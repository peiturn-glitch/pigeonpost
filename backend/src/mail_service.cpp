#include "mail_service.hpp"

#include "errors.hpp"
#include "smtp.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>

namespace tbk {

namespace {

constexpr auto kIdleCheckAfter = std::chrono::seconds(60);
constexpr std::size_t kRawCacheEntries = 8;

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
    if (it == known.end()) throw BadRequest("Okänd flagga: " + name);
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

std::string specialUseOf(const FolderInfo& f) {
    if (f.path == "INBOX") return "inbox";
    static const std::map<std::string, std::string> attrs = {
        {"\\sent", "sent"}, {"\\drafts", "drafts"}, {"\\trash", "trash"},
        {"\\junk", "junk"}, {"\\archive", "archive"}, {"\\all", "all"}, {"\\flagged", "flagged"}};
    for (const auto& [attr, use] : attrs) {
        if (f.attributes.count(attr)) return use;
    }
    return specialUseByName(f.path, f.delimiter);
}

}  // namespace

std::shared_ptr<MailService::Session> MailService::session(long long accountId) {
    std::lock_guard lock(sessionsMutex_);
    auto& s = sessions_[accountId];
    if (!s) s = std::make_shared<Session>();
    return s;
}

Account MailService::account(long long accountId) {
    auto acc = db_.getAccount(accountId);
    if (!acc) throw NotFound("Kontot finns inte");
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
        try {
            s->client->noop();
        } catch (const std::exception&) {
            s->client.reset();
        }
    }
    auto connect = [&]() {
        Account acc = account(accountId);
        auto client = std::make_unique<ImapClient>();
        client->connect(acc.imapHost, acc.imapPort, parseSecurity(acc.imapSecurity), acc.tlsVerify);
        login(*client, acc);
        s->client = std::move(client);
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
        std::cerr << "[imap] konto " << accountId << ": " << e.what() << "; ansluter på nytt\n";
    }
    connect();
    try {
        return fn(*s->client, *s);
    } catch (const NetError&) {
        s->client.reset();
        throw;
    }
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

json MailService::listFolders(long long accountId) {
    return withImap(accountId, [&](ImapClient& client, Session& s) {
        auto folders = client.listFolders(true);
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
    if (path.empty()) throw BadRequest("Mappnamn saknas");
    withImap(accountId, [&](ImapClient& client, Session& s) {
        client.createFolder(path);
        s.specialResolved = false;
    });
}

void MailService::deleteFolder(long long accountId, const std::string& path) {
    if (path.empty() || path == "INBOX") throw BadRequest("Den mappen kan inte tas bort");
    withImap(accountId, [&](ImapClient& client, Session& s) {
        client.deleteFolder(path);
        s.specialResolved = false;
        s.rawCache.clear();
    });
}

void MailService::renameFolder(long long accountId, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == "INBOX") throw BadRequest("Ogiltigt mappnamn");
    withImap(accountId, [&](ImapClient& client, Session& s) {
        client.renameFolder(from, to);
        s.specialResolved = false;
        s.rawCache.clear();
    });
}

json MailService::listMessages(long long accountId, const MessageQuery& q) {
    return withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(q.folder);

        ImapCommand criteria("ALL");
        if (q.unreadOnly) criteria.raw(" UNSEEN");
        if (q.flaggedOnly) criteria.raw(" FLAGGED");
        if (!q.text.empty()) {
            criteria.raw(" OR OR SUBJECT ").str(q.text).raw(" FROM ").str(q.text).raw(" TO ").str(q.text);
        }
        auto uids = client.uidSearch(criteria, !isAscii(q.text));
        std::sort(uids.begin(), uids.end(), std::greater<>());

        json result = {{"total", uids.size()}, {"page", q.page}, {"pageSize", q.pageSize}};
        json messages = json::array();

        std::size_t start = static_cast<std::size_t>(q.page) * static_cast<std::size_t>(q.pageSize);
        if (start < uids.size()) {
            std::size_t end = std::min(uids.size(), start + static_cast<std::size_t>(q.pageSize));
            std::vector<std::uint32_t> pageUids(uids.begin() + static_cast<long>(start),
                                                uids.begin() + static_cast<long>(end));
            auto fetched = client.uidFetch(ImapClient::uidSetFrom(pageUids), kHeaderFetch);
            std::sort(fetched.begin(), fetched.end(),
                      [](const FetchedMessage& a, const FetchedMessage& b) { return a.uid > b.uid; });
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

std::string MailService::rawMessage(ImapClient& client, Session& s, const MessageRef& ref) {
    std::string key = ref.folder + '\n' + std::to_string(ref.uid);
    for (const auto& [k, raw] : s.rawCache) {
        if (k == key) return raw;
    }
    client.select(ref.folder);
    auto fetched = client.uidFetch(std::to_string(ref.uid), "(UID BODY.PEEK[])");
    if (fetched.empty()) throw NotFound("Meddelandet finns inte längre");
    s.rawCache.emplace_front(key, fetched.front().body);
    while (s.rawCache.size() > kRawCacheEntries) s.rawCache.pop_back();
    return fetched.front().body;
}

json MailService::getMessage(long long accountId, const MessageRef& ref, bool markSeen) {
    return withImap(accountId, [&](ImapClient& client, Session& s) {
        std::string raw = rawMessage(client, s, ref);
        client.select(ref.folder);
        auto flagsFetch = client.uidFetch(std::to_string(ref.uid), "(UID FLAGS)");
        if (flagsFetch.empty()) throw NotFound("Meddelandet finns inte längre");
        auto flags = flagsFetch.front().flags;
        if (markSeen && !flags.count("\\seen")) {
            client.uidStore(std::to_string(ref.uid), "+FLAGS.SILENT", "\\Seen");
            flags.insert("\\seen");
        }

        auto msg = mime::parseMessage(raw);
        json attachments = json::array();
        for (const auto& a : msg.attachments) {
            attachments.push_back({
                {"index", a.index},
                {"filename", a.filename},
                {"contentType", a.contentType},
                {"size", a.data.size()},
                {"inline", a.inlineInHtml},
            });
        }
        return json{
            {"uid", ref.uid},
            {"folder", ref.folder},
            {"subject", msg.subject},
            {"from", addressesJson(msg.from)},
            {"to", addressesJson(msg.to)},
            {"cc", addressesJson(msg.cc)},
            {"replyTo", addressesJson(msg.replyTo)},
            {"date", msg.date},
            {"messageId", msg.messageId},
            {"references", msg.references},
            {"text", msg.text},
            {"html", msg.html},
            {"attachments", attachments},
            {"seen", flags.count("\\seen") > 0},
            {"flagged", flags.count("\\flagged") > 0},
            {"answered", flags.count("\\answered") > 0},
            {"draft", flags.count("\\draft") > 0},
        };
    });
}

mime::Attachment MailService::getAttachment(long long accountId, const MessageRef& ref, int index) {
    return withImap(accountId, [&](ImapClient& client, Session& s) {
        auto msg = mime::parseMessage(rawMessage(client, s, ref));
        if (index < 0 || static_cast<std::size_t>(index) >= msg.attachments.size()) {
            throw NotFound("Bilagan finns inte");
        }
        return std::move(msg.attachments[static_cast<std::size_t>(index)]);
    });
}

std::string MailService::getSource(long long accountId, const MessageRef& ref) {
    return withImap(accountId, [&](ImapClient& client, Session& s) { return rawMessage(client, s, ref); });
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
}

void MailService::moveMessages(long long accountId, const std::string& folder,
                               const std::vector<std::uint32_t>& uids, const std::string& target) {
    if (uids.empty()) return;
    if (target.empty() || target == folder) throw BadRequest("Ogiltig målmapp");
    withImap(accountId, [&](ImapClient& client, Session&) {
        client.select(folder);
        client.uidMove(ImapClient::uidSetFrom(uids), target);
    });
}

void MailService::deleteMessages(long long accountId, const std::string& folder,
                                 const std::vector<std::uint32_t>& uids) {
    if (uids.empty()) return;
    withImap(accountId, [&](ImapClient& client, Session& s) {
        auto trash = specialFolder(client, s, "trash");
        client.select(folder);
        std::string set = ImapClient::uidSetFrom(uids);
        if (trash && *trash != folder) {
            client.uidMove(set, *trash);
        } else {
            // I papperskorgen (eller om den saknas) raderas meddelandena permanent.
            client.uidStore(set, "+FLAGS.SILENT", "\\Deleted");
            client.expungeUids(set);
        }
    });
}

Account MailService::applySender(const Account& acc, mime::OutgoingMessage& message, const SendOptions& options) {
    message.fromName = acc.name;
    message.fromEmail = acc.email;
    if (!options.identityId) return acc;

    auto identity = db_.getIdentity(*options.identityId);
    if (!identity || identity->accountId != acc.id) throw BadRequest("Avsändaren hör inte till kontot");
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
            auto original = mime::parseMessage(rawMessage(client, s, *options.forwardOf));
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
                } catch (const MailServerError& e) {
                    warnings.push_back(std::string("Kunde inte spara i Skickat: ") + e.what());
                }
            } else {
                warnings.push_back("Ingen Skickat-mapp hittades; en kopia sparades inte.");
            }
            if (options.replyTo) {
                client.select(options.replyTo->folder);
                client.uidStore(std::to_string(options.replyTo->uid), "+FLAGS.SILENT", "\\Answered");
            }
            if (options.draft) {
                client.select(options.draft->folder);
                std::string uid = std::to_string(options.draft->uid);
                client.uidStore(uid, "+FLAGS.SILENT", "\\Deleted");
                client.expungeUids(uid);
            }
        });
    } catch (const std::exception& e) {
        warnings.push_back(std::string("Efterbehandling misslyckades: ") + e.what());
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
            drafts = "Drafts";
        }
        auto uid = client.append(*drafts, "\\Draft \\Seen", built.data);
        if (options.draft) {
            try {
                client.select(options.draft->folder);
                std::string old = std::to_string(options.draft->uid);
                client.uidStore(old, "+FLAGS.SILENT", "\\Deleted");
                client.expungeUids(old);
            } catch (const MailServerError& e) {
                std::cerr << "[drafts] kunde inte ta bort gammalt utkast: " << e.what() << "\n";
            }
        }
        json out = {{"folder", *drafts}};
        out["uid"] = uid ? json(*uid) : json(nullptr);
        return out;
    });
}

}  // namespace tbk
