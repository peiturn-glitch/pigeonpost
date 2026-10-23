#include "mail_service.hpp"

#include "errors.hpp"
#include "mail_cache.hpp"
#include "smtp.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>

namespace brevduva {

namespace {

constexpr auto kIdleCheckAfter = std::chrono::seconds(60);
// Ordinarie tidsgräns per läsning/skrivning mot IMAP-servern, och den kortare som gäller
// när en återanvänd anslutning kontrolleras med NOOP: ett NOOP-svar ska komma på en bråkdels
// sekund, så svarar servern inte på några sekunder är anslutningen död.
constexpr int kImapTimeoutSeconds = 30;
constexpr int kLivenessTimeoutSeconds = 5;
// Uppkopplingar som tar längre än så här loggas, så att tröga mejlservrar syns i loggen.
constexpr auto kSlowConnectLog = std::chrono::seconds(3);

const char* kHeaderFetch =
    "(UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (FROM TO SUBJECT DATE CONTENT-TYPE)])";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

json addressJson(const mime::Address& a) {
    return {{"name", a.name}, {"email", a.email}};
}

json addressesJson(const std::vector<mime::Address>& list) {
    json out = json::array();
    for (const auto& a : list) out.push_back(addressJson(a));
    return out;
}

// Mappar klientens flaggnamn till IMAP-systemflaggor. Okända namn avvisas.
std::string imapFlag(const std::string& name) {
    static const std::map<std::string, std::string> known = {
        {"seen", "\\Seen"}, {"flagged", "\\Flagged"}, {"answered", "\\Answered"}, {"draft", "\\Draft"}};
    auto it = known.find(name);
    if (it == known.end()) throw BadRequest("Unknown flag: " + name);
    return it->second;
}

bool isAscii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; });
}

// Känner igen specialmappar på namn när servern saknar SPECIAL-USE.
std::string specialUseByName(const std::string& path, const std::string& delimiter) {
    std::string name = path;
    if (!delimiter.empty()) {
        std::size_t pos = path.rfind(delimiter);
        if (pos != std::string::npos) name = path.substr(pos + delimiter.size());
    }
    name = lower(name);
    static const std::map<std::string, std::set<std::string>> names = {
        {"sent", {"sent", "sent items", "sent messages", "sent mail", "skickat", "skickade",
                  "skickade objekt", "skickade meddelanden"}},
        {"drafts", {"drafts", "draft", "utkast"}},
        {"trash", {"trash", "deleted", "deleted items", "deleted messages", "bin", "papperskorg",
                   "papperskorgen", "borttaget", "borttagna objekt"}},
        {"junk", {"junk", "spam", "junk e-mail", "junk email", "skräppost", "skräp"}},
        {"archive", {"archive", "archives", "arkiv"}},
    };
    for (const auto& [use, candidates] : names) {
        if (candidates.count(name)) return use;
    }
    return "";
}

// Kontot med identitetens SMTP-inställningar, om den har egna. IMAP-delen används inte vid sändning.
Account smtpSettingsFor(const Account& acc, const Identity& identity) {
    if (identity.useAccountSmtp) return acc;
    Account smtp = acc;
    smtp.smtpHost = identity.smtpHost;
    smtp.smtpPort = identity.smtpPort;
    smtp.smtpSecurity = identity.smtpSecurity;
    smtp.username = identity.smtpUsername;  // Tomt = ingen SMTP-inloggning.
    smtp.smtpUsername = identity.smtpUsername;
    smtp.password = identity.smtpPassword;
    smtp.tlsVerify = identity.tlsVerify;
    // En egen SMTP-server loggar alltid in med lösenord, även om kontot använder Google-inloggning.
    smtp.authType = "password";
    smtp.oauthRefreshToken.clear();
    smtp.oauthAccessToken.clear();
    return smtp;
}

}  // namespace

std::string specialUseOf(const FolderInfo& f) {
    if (f.path == "INBOX") return "inbox";
    static const std::map<std::string, std::string> attrs = {
        {"\\sent", "sent"}, {"\\drafts", "drafts"}, {"\\trash", "trash"},
        {"\\junk", "junk"}, {"\\archive", "archive"}, {"\\all", "all"}, {"\\flagged", "flagged"},
        // Gmails "Viktigt" är liksom "Alla meddelanden" och "Stjärnmärkt" en vy över mejl som ligger i andra mappar.
        {"\\important", "important"}};
    for (const auto& [attr, use] : attrs) {
        if (f.attributes.count(attr)) return use;
    }
    return specialUseByName(f.path, f.delimiter);
}

std::shared_ptr<MailService::Session> MailService::session(long long accountId) {
    std::lock_guard lock(sessionsMutex_);
    auto& s = sessions_[accountId];
    if (!s) s = std::make_shared<Session>();
    return s;
}

Account MailService::account(long long accountId) {
    auto acc = db_.getAccount(accountId);
    if (!acc) throw NotFound("The account does not exist");
    return *acc;
}

void MailService::dropSession(long long accountId) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(sessionsMutex_);
        auto it = sessions_.find(accountId);
        if (it == sessions_.end()) return;
        s = it->second;
        sessions_.erase(it);
    }
    std::lock_guard lock(s->mutex);
    if (s->client) s->client->logout();
}

template <typename F>
auto MailService::withImap(long long accountId, F&& fn) {
    auto s = session(accountId);
    std::lock_guard lock(s->mutex);
    auto now = std::chrono::steady_clock::now();

    // En återanvänd anslutning kan ha stängts av servern; kontrollera med NOOP om den legat oanvänd.
    if (s->client && s->client->connected() && now - s->lastUse > kIdleCheckAfter) {
        s->client->setTimeout(kLivenessTimeoutSeconds);
        try {
            s->client->noop();
            s->client->setTimeout(kImapTimeoutSeconds);
        } catch (const std::exception& e) {
            std::cerr << "[imap] account " << accountId << ": the connection did not respond to NOOP (" << e.what()
                      << "); reconnecting\n";
            s->client.reset();
        }
    }
    auto connect = [&]() {
        auto start = std::chrono::steady_clock::now();
        s->client = connectImap(account(accountId));
        auto took = std::chrono::steady_clock::now() - start;
        if (took > kSlowConnectLog) {
            std::cerr << "[imap] account " << accountId << ": connecting took "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() / 1000.0 << " s\n";
        }
    };

    bool reused = s->client && s->client->connected();
    if (!reused) connect();
    s->lastUse = now;
    try {
        return fn(*s->client, *s);
    } catch (const NetError& e) {
        s->client.reset();
        if (!reused) throw;
        // Servern har stängt en återanvänd anslutning (t.ex. autologout). Kommandot nådde då inte
        // fram, så det är säkert att försöka en gång till på en ny anslutning.
        std::cerr << "[imap] account " << accountId << ": " << e.what() << "; reconnecting\n";
    }
    connect();
    try {
        return fn(*s->client, *s);
    } catch (const NetError&) {
        s->client.reset();
        throw;
    }
}

std::unique_ptr<ImapClient> MailService::connectImap(const Account& acc) {
    auto client = std::make_unique<ImapClient>();
    client->connect(acc.imapHost, acc.imapPort, parseSecurity(acc.imapSecurity), acc.tlsVerify);
    login(*client, acc);
    return client;
}

void MailService::login(ImapClient& client, const Account& acc) {
    if (acc.usesGoogleOAuth()) {
        client.loginXOAuth2(acc.username, oauth_.accessToken(acc));
    } else {
        client.login(acc.username, acc.password);
    }
}

Account MailService::withAccessToken(Account acc) {
    if (acc.usesGoogleOAuth() && acc.oauthAccessToken.empty()) acc.oauthAccessToken = oauth_.accessToken(acc);
    return acc;
}

json MailService::testAccount(const Account& acc) {
    json result;
    try {
        ImapClient client;
        client.connect(acc.imapHost, acc.imapPort, parseSecurity(acc.imapSecurity), acc.tlsVerify);
        login(client, acc);
        client.logout();
        result["imap"] = {{"ok", true}};
    } catch (const std::exception& e) {
        result["imap"] = {{"ok", false}, {"error", e.what()}};
    }
    try {
        smtpTest(withAccessToken(acc));
        result["smtp"] = {{"ok", true}};
    } catch (const std::exception& e) {
        result["smtp"] = {{"ok", false}, {"error", e.what()}};
    }
    return result;
}

void MailService::resolveSpecialFolders(Session& s, const std::vector<FolderInfo>& folders) {
    s.specialFolders.clear();
    for (const auto& f : folders) {
        std::string use = specialUseOf(f);
        if (!use.empty() && !s.specialFolders.count(use)) s.specialFolders[use] = f.path;
    }
    s.specialResolved = true;
}

std::optional<std::string> MailService::specialFolder(ImapClient& client, Session& s, const std::string& use) {
    if (!s.specialResolved) resolveSpecialFolders(s, client.listFolders(false));
    auto it = s.specialFolders.find(use);
    if (it == s.specialFolders.end()) return std::nullopt;
    return it->second;
}

json MailService::listFolders(long long accountId, bool withStatus) {
    if (cache_) {
        if (auto cached = cache_->folders(accountId, false)) return *cached;
    }
    try {
        return listFoldersLive(accountId, withStatus);
    } catch (const std::exception& e) {
        // Mejlservern svarar inte: visa den mapplista som finns sparad.
        if (cache_) {
            if (dynamic_cast<const NetError*>(&e)) cache_->reportUnreachable(accountId, e.what());
            if (auto cached = cache_->folders(accountId, true)) return *cached;
        }
        throw;
    }
}

json MailService::listFoldersLive(long long accountId, bool withStatus) {
    return withImap(accountId, [&](ImapClient& client, Session& s) {
        auto folders = client.listFolders(withStatus);
        resolveSpecialFolders(s, folders);
        json out = json::array();
        for (const auto& f : folders) {
            if (f.attributes.count("\\nonexistent")) continue;
            out.push_back({
                {"path", f.path},
                {"delimiter", f.delimiter},
                {"specialUse", specialUseOf(f)},
                {"selectable", !f.attributes.count("\\noselect")},
                {"total", f.total},
                {"unread", f.unread},
            });
        }
        return out;
    });
}

void MailService::createFolder(long long accountId, const std::string& path) {
    if (path.empty()) throw BadRequest("Folder name is missing");
    withImap(accountId, [&](ImapClient& client, Session& s) {
        client.createFolder(path);
        s.specialResolved = false;
    });
    if (cache_) cache_->foldersChanged(accountId);
}

void MailService::deleteFolder(long long accountId, const std::string& path) {
    if (path.empty() || path == "INBOX") throw BadRequest("That folder cannot be deleted");
    withImap(accountId, [&](ImapClient& client, Session& s) {
        client.deleteFolder(path);
        s.specialResolved = false;
    });
    if (cache_) cache_->folderDeleted(accountId, path);
}

void MailService::renameFolder(long long accountId, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == "INBOX") throw BadRequest("Ogiltigt mappnamn");
    if (to.size() > from.size() && to.compare(0, from.size(), from) == 0 && (to[from.size()] == '/' || to[from.size()] == '.')) {
        throw BadRequest("A folder cannot be moved into itself");
    }
    withImap(accountId, [&](ImapClient& client, Session& s) {
        client.renameFolder(from, to);
        s.specialResolved = false;
    });
    if (cache_) cache_->folderRenamed(accountId, from, to);
}

json MailService::listMessages(long long accountId, const MessageQuery& q) {
    if (cache_) {
        if (auto cached = cache_->messages(accountId, q, false)) return *cached;
        // Mejlservern går inte att nå: visa det som finns, hellre än att vänta på en anslutning som inte kommer.
        if (cache_->offline(accountId)) {
            if (auto stale = cache_->messages(accountId, q, true)) return *stale;
        }
    }
    try {
        return listMessagesLive(accountId, q);
    } catch (const std::exception& e) {
        if (cache_) {
            if (dynamic_cast<const NetError*>(&e)) cache_->reportUnreachable(accountId, e.what());
            if (auto stale = cache_->messages(accountId, q, true)) return *stale;
        }
        throw;
    }
}

json MailService::listMessagesLive(long long accountId, const MessageQuery& q) {
    return withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(q.folder);

        ImapCommand criteria("ALL");
        if (q.beforeUid > 1) criteria.raw(" UID 1:" + std::to_string(q.beforeUid - 1));
        if (q.unreadOnly) criteria.raw(" UNSEEN");
        if (q.flaggedOnly) criteria.raw(" FLAGGED");
        if (!q.text.empty()) {
            criteria.raw(" OR OR SUBJECT ").str(q.text).raw(" FROM ").str(q.text).raw(" TO ").str(q.text);
        }
        // Servern sorterar om den kan (SORT-tillägget). Annars gäller ankomstordningen, som för det mesta är
        // datumordning; Gmail saknar t.ex. SORT.
        static const std::map<std::string, std::string> sortKeys = {
            {"date", "DATE"}, {"subject", "SUBJECT"}, {"from", "FROM"}, {"to", "TO"}, {"size", "SIZE"}};
        auto key = sortKeys.find(q.sort);
        std::vector<std::uint32_t> uids;
        if (key != sortKeys.end() && client.hasCapability("SORT")) {
            uids = client.uidSort(std::string(q.descending ? "REVERSE " : "") + key->second, criteria, !isAscii(q.text));
        } else {
            uids = client.uidSearch(criteria, !isAscii(q.text));
            if (q.descending) std::sort(uids.begin(), uids.end(), std::greater<>());
            else std::sort(uids.begin(), uids.end());
        }
        if (q.beforeUid == 1) uids.clear();  // Inget UID är lägre än 1, och "UID 1:0" är inte giltig IMAP.

        json result = {{"total", uids.size()}, {"page", q.page}, {"pageSize", q.pageSize}};
        json messages = json::array();

        std::size_t start = static_cast<std::size_t>(q.page) * static_cast<std::size_t>(q.pageSize);
        if (start < uids.size()) {
            std::size_t end = std::min(uids.size(), start + static_cast<std::size_t>(q.pageSize));
            std::vector<std::uint32_t> pageUids(uids.begin() + static_cast<long>(start),
                                                uids.begin() + static_cast<long>(end));
            auto fetched = client.uidFetch(ImapClient::uidSetFrom(pageUids), kHeaderFetch);
            // Servern svarar i sin egen ordning; sidans ordning är den sorterade.
            std::map<std::uint32_t, std::size_t> position;
            for (std::size_t k = 0; k < pageUids.size(); ++k) position[pageUids[k]] = k;
            std::sort(fetched.begin(), fetched.end(),
                      [&](const FetchedMessage& a, const FetchedMessage& b) { return position[a.uid] < position[b.uid]; });
            for (const auto& m : fetched) {
                auto h = mime::parseHeaders(m.body, m.internalDate);
                messages.push_back({
                    {"uid", m.uid},
                    {"subject", h.subject},
                    {"from", addressesJson(h.from)},
                    {"to", addressesJson(h.to)},
                    {"date", h.date},
                    {"size", m.size},
                    {"seen", m.flags.count("\\seen") > 0},
                    {"flagged", m.flags.count("\\flagged") > 0},
                    {"answered", m.flags.count("\\answered") > 0},
                    {"draft", m.flags.count("\\draft") > 0},
                    {"hasAttachments", h.hasAttachments},
                });
            }
        }
        result["messages"] = std::move(messages);
        return result;
    });
}

std::string MailService::rawMessage(long long accountId, ImapClient& client, const MessageRef& ref) {
    if (auto cached = rawCache_.get(accountId, ref.folder, ref.uid)) return *cached;
    client.select(ref.folder);
    auto fetched = client.uidFetch(std::to_string(ref.uid), "(UID BODY.PEEK[])");
    if (fetched.empty()) throw NotFound("The message no longer exists");
    rawCache_.put(accountId, ref.folder, ref.uid, fetched.front().body);
    return fetched.front().body;
}

json MailService::getMessage(long long accountId, const MessageRef& ref, bool markSeen) {
    if (cache_) {
        auto cached = cache_->message(accountId, ref, false);
        if (cached) {
            if (markSeen && !(*cached)["seen"].get<bool>()) {
                // Läsningen ska inte vänta på mejlservern: cachen uppdateras nu och servern strax efteråt.
                (*cached)["seen"] = true;
                cache_->flagsChanged(accountId, ref.folder, {ref.uid}, {"seen"}, {});
                cache_->markSeenLater(accountId, ref);
            }
            return *cached;
        }
        if (cache_->offline(accountId)) {
            throw MailServerError("The mail server cannot be reached, and this message has not yet been saved to the cache.");
        }
    }
    try {
        return getMessageLive(accountId, ref, markSeen);
    } catch (const NetError& e) {
        if (cache_) {
            cache_->reportUnreachable(accountId, e.what());
            if (auto stale = cache_->message(accountId, ref, true)) return *stale;
        }
        throw;
    }
}

json MailService::getMessageLive(long long accountId, const MessageRef& ref, bool markSeen) {
    return withImap(accountId, [&](ImapClient& client, Session&) {
        std::string raw = rawMessage(accountId, client, ref);
        client.select(ref.folder);
        auto flagsFetch = client.uidFetch(std::to_string(ref.uid), "(UID FLAGS)");
        if (flagsFetch.empty()) throw NotFound("The message no longer exists");
        auto flags = flagsFetch.front().flags;
        bool markedSeen = false;
        if (markSeen && !flags.count("\\seen")) {
            client.uidStore(std::to_string(ref.uid), "+FLAGS.SILENT", "\\Seen");
            flags.insert("\\seen");
            markedSeen = true;
        }

        // Inbäddade bilder skickas inte med i HTML-delen; klienten hämtar dem som bilagor.
        auto msg = mime::parseMessage(raw, false);
        if (cache_) {
            cache_->storeBody(accountId, ref, msg);
            if (markedSeen) cache_->flagsChanged(accountId, ref.folder, {ref.uid}, {"seen"}, {});
        }
        json out = MailCache::bodyJson(msg);
        out["uid"] = ref.uid;
        out["folder"] = ref.folder;
        out["subject"] = msg.subject;
        out["from"] = addressesJson(msg.from);
        out["to"] = addressesJson(msg.to);
        out["date"] = msg.date;
        out["text"] = msg.text;
        out["html"] = msg.html;
        out["seen"] = flags.count("\\seen") > 0;
        out["flagged"] = flags.count("\\flagged") > 0;
        out["answered"] = flags.count("\\answered") > 0;
        out["draft"] = flags.count("\\draft") > 0;
        return out;
    });
}

std::vector<MessageSummary> MailService::summarize(long long accountId, const std::string& folder,
                                                   const std::vector<std::uint32_t>& uids, std::size_t bodyBytes) {
    if (uids.empty()) return {};
    return withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(folder);
        std::string set = ImapClient::uidSetFrom(uids);
        auto headers = client.uidFetch(set, "(UID FLAGS INTERNALDATE BODY.PEEK[HEADER])");
        std::map<std::uint32_t, std::string> bodies;
        for (auto& m : client.uidFetch(set, "(UID BODY.PEEK[TEXT]<0." + std::to_string(bodyBytes) + ">)")) {
            bodies[m.uid] = std::move(m.body);
        }

        std::vector<MessageSummary> out;
        for (const auto& m : headers) {
            auto h = mime::parseHeaders(m.body, m.internalDate);
            MessageSummary summary;
            summary.uid = m.uid;
            summary.subject = h.subject;
            summary.from = h.from;
            summary.to = h.to;
            summary.date = h.date;
            summary.seen = m.flags.count("\\seen") > 0;
            try {
                // Rubrikerna ska sluta med en tomrad, men vissa servrar (GreenMail) utelämnar den.
                std::string raw = m.body;
                while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
                raw += "\r\n\r\n" + bodies[m.uid];
                // Innehållet är avklippt, men GMime tolkar det som hunnit komma med.
                auto parsed = mime::parseMessage(raw);
                summary.messageId = std::move(parsed.messageId);
                summary.text = std::move(parsed.text);
                summary.html = std::move(parsed.html);
            } catch (const std::exception&) {
                // Rubrikerna räcker om innehållet inte går att tolka.
            }
            out.push_back(std::move(summary));
        }
        return out;
    });
}

NewMessages MailService::newMessages(long long accountId, const std::string& folder, std::uint32_t afterUid,
                                     std::size_t limit) {
    return withImap(accountId, [&](ImapClient& client, Session&) {
        MailboxStatus status = client.status(folder);
        NewMessages out;
        out.uidValidity = status.uidValidity;
        out.uidNext = status.uidNext;
        if (status.uidNext <= afterUid + 1) return out;

        client.select(folder);
        // "n:*" ger alltid mappens sista meddelande, även när dess UID är lägre än n.
        for (auto uid : client.uidSearch(ImapCommand("UID " + std::to_string(afterUid + 1) + ":*"), false)) {
            if (uid > afterUid) out.uids.push_back(uid);
        }
        std::sort(out.uids.begin(), out.uids.end());
        if (out.uids.size() > limit) out.uids.resize(limit);
        return out;
    });
}

bool MailService::moveByMessageId(long long accountId, const std::string& folder, const std::string& messageId,
                                  const std::string& target) {
    if (messageId.empty() || target.empty() || target == folder) throw BadRequest("Invalid move");
    std::vector<std::uint32_t> moved;
    withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(folder);
        ImapCommand criteria("HEADER Message-ID ");
        criteria.str(messageId);
        moved = client.uidSearch(criteria, !isAscii(messageId));
        if (!moved.empty()) client.uidMove(ImapClient::uidSetFrom(moved), target);
    });
    if (cache_ && !moved.empty()) cache_->messagesRemoved(accountId, folder, moved, target);
    return !moved.empty();
}

// Ett meddelande som öppnats nyligen ligger på disk och kräver ingen kontakt med mejlservern.
std::string MailService::rawMessageCached(long long accountId, const MessageRef& ref) {
    if (auto cached = rawCache_.get(accountId, ref.folder, ref.uid)) return *cached;
    return withImap(accountId, [&](ImapClient& client, Session&) { return rawMessage(accountId, client, ref); });
}

mime::Attachment MailService::getAttachment(long long accountId, const MessageRef& ref, int index) {
    auto msg = mime::parseMessage(rawMessageCached(accountId, ref));
    if (index < 0 || static_cast<std::size_t>(index) >= msg.attachments.size()) {
        throw NotFound("The attachment does not exist");
    }
    return std::move(msg.attachments[static_cast<std::size_t>(index)]);
}

std::string MailService::getSource(long long accountId, const MessageRef& ref) {
    return rawMessageCached(accountId, ref);
}

void MailService::setFlags(long long accountId, const std::string& folder, const std::vector<std::uint32_t>& uids,
                           const std::vector<std::string>& add, const std::vector<std::string>& remove) {
    if (uids.empty()) return;
    auto join = [](const std::vector<std::string>& names) {
        std::string out;
        for (const auto& n : names) out += (out.empty() ? "" : " ") + imapFlag(n);
        return out;
    };
    std::string addFlags = join(add);
    std::string removeFlags = join(remove);
    withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(folder);
        std::string set = ImapClient::uidSetFrom(uids);
        if (!addFlags.empty()) client.uidStore(set, "+FLAGS.SILENT", addFlags);
        if (!removeFlags.empty()) client.uidStore(set, "-FLAGS.SILENT", removeFlags);
    });
    if (cache_) cache_->flagsChanged(accountId, folder, uids, add, remove);
}

void MailService::moveMessages(long long accountId, const std::string& folder,
                               const std::vector<std::uint32_t>& uids, const std::string& target) {
    if (uids.empty()) return;
    if (target.empty() || target == folder) throw BadRequest("Invalid target folder");
    withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(folder);
        client.uidMove(ImapClient::uidSetFrom(uids), target);
    });
    if (cache_) cache_->messagesRemoved(accountId, folder, uids, target);
}

void MailService::deleteMessages(long long accountId, const std::string& folder,
                                 const std::vector<std::uint32_t>& uids) {
    if (uids.empty()) return;
    std::string movedTo;
    withImap(accountId, [&](ImapClient& client, Session& s) {
        auto trash = specialFolder(client, s, "trash");
        client.select(folder);
        std::string set = ImapClient::uidSetFrom(uids);
        if (trash && *trash != folder) {
            client.uidMove(set, *trash);
            movedTo = *trash;
        } else {
            // I papperskorgen (eller om den saknas) raderas meddelandena permanent.
            client.uidStore(set, "+FLAGS.SILENT", "\\Deleted");
            client.expungeUids(set);
        }
    });
    if (cache_) cache_->messagesRemoved(accountId, folder, uids, movedTo);
}

Account MailService::applySender(const Account& acc, mime::OutgoingMessage& message, const SendOptions& options) {
    message.fromName = acc.name;
    message.fromEmail = acc.email;
    if (!options.identityId) return acc;

    auto identity = db_.getIdentity(*options.identityId);
    if (!identity || identity->accountId != acc.id) throw BadRequest("The sender does not belong to the account");
    message.fromName = identity->name;
    message.fromEmail = identity->email;
    if (message.replyTo.empty()) message.replyTo = identity->replyTo;
    return smtpSettingsFor(acc, *identity);
}

json MailService::testIdentitySmtp(const Account& acc, const Identity& identity) {
    try {
        smtpTest(withAccessToken(smtpSettingsFor(acc, identity)));
        return {{"ok", true}};
    } catch (const std::exception& e) {
        return {{"ok", false}, {"error", e.what()}};
    }
}

json MailService::send(long long accountId, mime::OutgoingMessage message, const SendOptions& options) {
    Account acc = account(accountId);
    Account smtpAccount = applySender(acc, message, options);

    if (options.forwardOf) {
        withImap(accountId, [&](ImapClient& client, Session& s) {
            auto original = mime::parseMessage(rawMessage(accountId, client, *options.forwardOf));
            for (auto& a : original.attachments) {
                if (a.inlineInHtml) continue;
                message.attachments.push_back({a.filename, a.contentType, std::move(a.data)});
            }
        });
    }

    auto built = mime::buildMessage(message);
    smtpSend(withAccessToken(smtpAccount), message.fromEmail, built.recipients, built.data);

    // Meddelandet är skickat; fel efter detta rapporteras som varningar.
    json warnings = json::array();
    try {
        withImap(accountId, [&](ImapClient& client, Session& s) {
            if (auto sent = specialFolder(client, s, "sent")) {
                try {
                    client.append(*sent, "\\Seen", built.data);
                    if (cache_) cache_->folderTouched(accountId, *sent);
                } catch (const MailServerError& e) {
                    warnings.push_back(std::string("Could not save to Sent: ") + e.what());
                }
            } else {
                warnings.push_back("No Sent folder was found; a copy was not saved.");
            }
            if (options.replyTo) {
                client.select(options.replyTo->folder);
                client.uidStore(std::to_string(options.replyTo->uid), "+FLAGS.SILENT", "\\Answered");
                if (cache_) cache_->flagsChanged(accountId, options.replyTo->folder, {options.replyTo->uid}, {"answered"}, {});
            }
            if (options.draft) {
                client.select(options.draft->folder);
                std::string uid = std::to_string(options.draft->uid);
                client.uidStore(uid, "+FLAGS.SILENT", "\\Deleted");
                client.expungeUids(uid);
                if (cache_) cache_->messagesRemoved(accountId, options.draft->folder, {options.draft->uid}, "");
            }
        });
    } catch (const std::exception& e) {
        warnings.push_back(std::string("Post-processing failed: ") + e.what());
    }
    return {{"ok", true}, {"warnings", warnings}};
}

json MailService::saveDraft(long long accountId, mime::OutgoingMessage message, const SendOptions& options) {
    Account acc = account(accountId);
    applySender(acc, message, options);
    auto built = mime::buildMessage(message);

    return withImap(accountId, [&](ImapClient& client, Session& s) {
        auto drafts = specialFolder(client, s, "drafts");
        if (!drafts) {
            client.createFolder("Drafts");
            s.specialResolved = false;
            if (cache_) cache_->foldersChanged(accountId);
            drafts = "Drafts";
        }
        auto uid = client.append(*drafts, "\\Draft \\Seen", built.data);
        if (cache_) cache_->folderTouched(accountId, *drafts);
        if (options.draft) {
            try {
                client.select(options.draft->folder);
                std::string old = std::to_string(options.draft->uid);
                client.uidStore(old, "+FLAGS.SILENT", "\\Deleted");
                client.expungeUids(old);
                if (cache_) cache_->messagesRemoved(accountId, options.draft->folder, {options.draft->uid}, "");
            } catch (const MailServerError& e) {
                std::cerr << "[drafts] could not delete the old draft: " << e.what() << "\n";
            }
        }
        json out = {{"folder", *drafts}};
        out["uid"] = uid ? json(*uid) : json(nullptr);
        return out;
    });
}

}  // namespace brevduva
