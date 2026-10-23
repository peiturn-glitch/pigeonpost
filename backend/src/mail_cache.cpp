#include "mail_cache.hpp"

#include "errors.hpp"
#include "mail_service.hpp"
#include "raw_cache.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <set>

namespace brevduva {

namespace {

constexpr int kWorkers = 2;
constexpr std::int64_t kPassInterval = 20;      // Sekunder mellan kontroller av ett konto som är i fas.
constexpr std::int64_t kFolderListFresh = 180;  // Så länge duger cachens mapplista utan att fråga mejlservern.
// Flaggor som ändrats från en annan klient syns inte alltid på mappens antal, så allt stäms av med jämna mellanrum.
constexpr std::int64_t kInboxReconcile = 10 * 60;
constexpr std::int64_t kOtherReconcile = 60 * 60;
// Arbete per konto och pass, så att alla konton och mappar kommer framåt även under en första synk.
constexpr int kHeaderBudget = 5000;
constexpr int kHeaderChunk = 200;
constexpr int kBodyBudget = 200;
constexpr std::int64_t kBodyBytesBudget = 24 * 1024 * 1024;
constexpr std::int64_t kBodyBatchBytes = 4 * 1024 * 1024;
constexpr int kBodyBatchCount = 25;
// Större meddelanden än så hämtas bara till en del: texten ligger först, bilagorna sist.
constexpr std::int64_t kFullFetchMax = 512 * 1024;
constexpr std::size_t kMaxText = 1024 * 1024;
constexpr std::size_t kMaxHtml = 3 * 1024 * 1024;
constexpr std::size_t kMaxSearchText = 64 * 1024;
// Gravstenar (borttagna meddelanden för appens synk) rensas så här ofta och sparas så här länge.
constexpr std::int64_t kTombstonePrune = 3600;
constexpr std::int64_t kTombstoneKeep = 14 * 24 * 3600;

const char* kHeaderFetch =
    "(UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (FROM TO SUBJECT DATE CONTENT-TYPE MESSAGE-ID)])";
// Push-notiser: efter en omstart räknas mejl från den senaste halvtimmen som nya; annars allt sedan förra passet med
// tio minuters marginal, men aldrig äldre än ett dygn.
constexpr std::int64_t kFreshAfterStart = 30 * 60;
constexpr std::int64_t kFreshMargin = 10 * 60;
constexpr std::int64_t kFreshMax = 24 * 3600;

std::int64_t now() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// Millisekunder, för att avgöra om en mapp ändrades före eller efter att en avstämning började.
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

json addressesJson(const std::vector<mime::Address>& list) {
    json out = json::array();
    for (const auto& a : list) out.push_back({{"name", a.name}, {"email", a.email}});
    return out;
}

std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

json parseArray(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    return j.is_array() ? j : json::array();
}

CacheFlags flagsOf(const FetchedMessage& m) {
    CacheFlags f;
    f.seen = m.flags.count("\\seen") > 0;
    f.flagged = m.flags.count("\\flagged") > 0;
    f.answered = m.flags.count("\\answered") > 0;
    f.draft = m.flags.count("\\draft") > 0;
    return f;
}

CacheFlags flagsFromNames(const std::vector<std::string>& names) {
    CacheFlags f;
    for (const auto& n : names) {
        if (n == "seen") f.seen = true;
        if (n == "flagged") f.flagged = true;
        if (n == "answered") f.answered = true;
        if (n == "draft") f.draft = true;
    }
    return f;
}

// Inkorgen först och papperskorgen sist, så att det viktigaste blir läsbart först under en första synk.
int folderPriority(const CacheFolder& f) {
    static const std::map<std::string, int> order = {{"inbox", 0}, {"drafts", 1}, {"sent", 2}, {"archive", 4}, {"junk", 5}, {"trash", 6}};
    auto it = order.find(f.specialUse);
    return it == order.end() ? 3 : it->second;
}

json messageItem(const CacheMessage& m) {
    return {
        {"uid", m.uid},
        {"subject", m.subject},
        {"from", parseArray(m.sender)},
        {"to", parseArray(m.recipients)},
        {"date", m.date},
        {"size", m.size},
        {"seen", m.flags.seen},
        {"flagged", m.flags.flagged},
        {"answered", m.flags.answered},
        {"draft", m.flags.draft},
        {"hasAttachments", m.hasAttachments},
    };
}

}  // namespace

MailCache::MailCache(const Config& config, Database& db, MailService& mail, RawCache& rawCache, PushService* push)
    : config_(config), db_(db), mail_(mail), rawCache_(rawCache), push_(push) {}

namespace {

// Mappar där ny post kan ge notis: inte det användaren själv skickat eller sorterat bort.
bool notifiable(const CacheFolder& f) {
    return f.specialUse != "sent" && f.specialUse != "drafts" && f.specialUse != "trash" && f.specialUse != "junk";
}

}  // namespace

MailCache::~MailCache() {
    stop();
}

void MailCache::start() {
    std::lock_guard lock(mutex_);
    if (started_) return;
    started_ = true;
    for (int i = 0; i < kWorkers; ++i) threads_.emplace_back(&MailCache::worker, this, i);
    threads_.emplace_back(&MailCache::deferredWorker, this);
}

void MailCache::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopping_) return;
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

bool MailCache::cacheable(const CacheFolder& f) {
    // Virtuella mappar visar mejl som redan ligger i andra mappar. De cachas inte, så att samma mejl inte finns två gånger.
    return f.selectable && f.specialUse != "all" && f.specialUse != "flagged" && f.specialUse != "important";
}

// --- Läsning ----------------------------------------------------------------------

bool MailCache::offline(long long accountId) {
    std::lock_guard lock(mutex_);
    auto it = states_.find(accountId);
    return it != states_.end() && it->second.offline;
}

void MailCache::reportUnreachable(long long accountId, const std::string& error) {
    {
        std::lock_guard lock(mutex_);
        auto& st = states_[accountId];
        if (!st.offline) std::cerr << "[cache] konto " << accountId << ": " << error << "\n";
        st.offline = true;
        st.error = error;
        st.reconnect = true;
    }
    kick(accountId);
}

std::optional<json> MailCache::folders(long long accountId, bool force) {
    if (!force) {
        std::lock_guard lock(mutex_);
        auto it = states_.find(accountId);
        if (it == states_.end()) return std::nullopt;
        if (!it->second.offline && now() - it->second.lastFolderListAt > kFolderListFresh) return std::nullopt;
    }
    auto folders = db_.listCacheFolders(accountId);
    if (folders.empty()) return std::nullopt;
    json out = json::array();
    for (const auto& f : folders) {
        out.push_back({{"path", f.path},
                       {"delimiter", f.delimiter},
                       {"specialUse", f.specialUse},
                       {"selectable", f.selectable},
                       {"total", f.total},
                       {"unread", f.unread}});
    }
    return out;
}

std::optional<json> MailCache::messages(long long accountId, const MessageQuery& q, bool force) {
    auto folder = db_.getCacheFolder(accountId, q.folder);
    if (!folder || !cacheable(*folder) || !folder->headersComplete) return std::nullopt;
    // En mapp som just fått nytt innehåll via appen läses från mejlservern tills den har stämts av.
    if (folder->dirtyAt > 0 && !force && !offline(accountId)) return std::nullopt;

    CacheListQuery query;
    query.page = q.page;
    query.pageSize = q.pageSize;
    query.text = q.text;
    query.unreadOnly = q.unreadOnly;
    query.beforeUid = q.beforeUid;
    query.flaggedOnly = q.flaggedOnly;
    query.sort = q.sort;
    query.descending = q.descending;
    auto result = db_.listCacheMessages(folder->id, query);

    json messages = json::array();
    for (const auto& m : result.messages) messages.push_back(messageItem(m));
    return json{{"total", result.total}, {"page", q.page}, {"pageSize", q.pageSize}, {"messages", messages}, {"cached", true}};
}

json MailCache::search(const CacheSearchQuery& query) {
    auto result = db_.searchCacheMessages(query);
    json messages = json::array();
    for (const auto& hit : result.hits) {
        json item = messageItem(hit.message);
        item["accountId"] = hit.accountId;
        item["folder"] = hit.folder;
        messages.push_back(std::move(item));
    }
    return {{"total", result.total}, {"page", query.page}, {"pageSize", query.pageSize}, {"messages", messages}};
}

json MailCache::changes(long long accountId, std::int64_t since, int limit) {
    if (since < 0) throw BadRequest("Ogiltigt värde för since");
    CacheSeq seq = db_.cacheSeq();
    // Märket ska ligga mellan den äldsta sparade gravstenen och det senast utdelade numret. Ett märke över det
    // senaste numret betyder att databasen har återställts från en säkerhetskopia.
    if (since > 0 && (since < seq.pruned || since > seq.value)) {
        throw Gone("Synkmärket är för gammalt eller okänt; hämta om kontot");
    }
    json folders = json::array();
    for (const auto& f : db_.listCacheFolders(accountId)) {
        folders.push_back({{"path", f.path},
                           {"delimiter", f.delimiter},
                           {"specialUse", f.specialUse},
                           {"selectable", f.selectable},
                           {"total", f.total},
                           {"unread", f.unread},
                           {"cached", cacheable(f)},
                           {"headersComplete", f.headersComplete},
                           {"resetModseq", f.resetModseq}});
    }
    json out = {{"since", since}, {"modseq", seq.value}, {"more", false}, {"folders", folders},
                {"messages", json::array()}, {"removed", json::array()}};
    if (since == 0 || since >= seq.value) return out;

    auto changes = db_.listCacheChanges(accountId, since, seq.value, limit);
    for (const auto& c : changes.messages) {
        json item = messageItem(c.message);
        item["folder"] = c.folder;
        item["modseq"] = c.message.modseq;
        out["messages"].push_back(std::move(item));
    }
    for (const auto& t : changes.removed) {
        out["removed"].push_back({{"folder", t.folder}, {"uid", t.uid}, {"modseq", t.modseq}});
    }
    out["modseq"] = changes.cutoff;
    out["more"] = changes.more;
    return out;
}

json MailCache::bodyJson(const mime::ParsedMessage& msg) {
    json attachments = json::array();
    for (const auto& a : msg.attachments) {
        attachments.push_back({
            {"index", a.index},
            {"filename", a.filename},
            {"contentType", a.contentType},
            {"size", a.data.size()},
            {"inline", a.inlineInHtml},
            // Inbäddade bilder hämtas av klienten som bilagor och sätts in där HTML-delen pekar på cid:<contentId>.
            {"contentId", a.contentId},
        });
    }
    return {
        {"cc", addressesJson(msg.cc)},
        {"replyTo", addressesJson(msg.replyTo)},
        {"messageId", msg.messageId},
        {"references", msg.references},
        {"attachments", attachments},
    };
}

std::optional<json> MailCache::message(long long accountId, const MessageRef& ref, bool force) {
    auto folder = db_.getCacheFolder(accountId, ref.folder);
    if (!folder) return std::nullopt;
    auto m = db_.getCacheMessage(folder->id, ref.uid);
    if (!m) return std::nullopt;
    // Ett delvis hämtat meddelande kan sakna bilagor i listan, så det läses hellre från mejlservern när den svarar.
    bool usable = m->bodyState == kBodyCached || (m->bodyState == kBodyPartial && (force || offline(accountId)));
    if (!usable) return std::nullopt;
    auto body = db_.getCacheBody(m->id);
    if (!body) return std::nullopt;

    json out = json::parse(body->meta, nullptr, false);
    if (!out.is_object()) return std::nullopt;
    out["uid"] = ref.uid;
    out["folder"] = ref.folder;
    out["subject"] = m->subject;
    out["from"] = parseArray(m->sender);
    out["to"] = parseArray(m->recipients);
    out["date"] = m->date;
    out["text"] = body->text;
    out["html"] = body->html;
    out["seen"] = m->flags.seen;
    out["flagged"] = m->flags.flagged;
    out["answered"] = m->flags.answered;
    out["draft"] = m->flags.draft;
    out["cached"] = true;
    out["partial"] = m->bodyState == kBodyPartial;
    return out;
}

// --- Ändringar ----------------------------------------------------------------------

namespace {

// Text, HTML och söktext för cachen. Returnerar false om meddelandet är för stort för att cachas.
bool buildBody(const mime::ParsedMessage& msg, CacheBody& body, std::string& searchText) {
    if (msg.text.size() > kMaxText || msg.html.size() > kMaxHtml) return false;
    body.meta = dump(MailCache::bodyJson(msg));
    body.text = msg.text;
    body.html = msg.html;
    std::string plain = msg.text.empty() ? textutil::htmlToText(msg.html) : msg.text;
    searchText = textutil::utf8Clean(textutil::truncateUtf8Bytes(textutil::collapseWhitespace(plain), kMaxSearchText));
    return true;
}

}  // namespace

void MailCache::storeBody(long long accountId, const MessageRef& ref, const mime::ParsedMessage& msg) {
    try {
        auto folder = db_.getCacheFolder(accountId, ref.folder);
        if (!folder) return;
        auto m = db_.getCacheMessage(folder->id, ref.uid);
        if (!m || m->bodyState == kBodyCached) return;
        CacheBody body;
        std::string searchText;
        if (buildBody(msg, body, searchText)) db_.saveCacheBody(m->id, kBodyCached, body, searchText);
    } catch (const std::exception& e) {
        std::cerr << "[cache] kunde inte spara meddelandetext: " << e.what() << "\n";
    }
}

void MailCache::flagsChanged(long long accountId, const std::string& folderPath, const std::vector<std::uint32_t>& uids,
                             const std::vector<std::string>& add, const std::vector<std::string>& remove) {
    try {
        auto folder = db_.getCacheFolder(accountId, folderPath);
        if (!folder) return;
        std::int64_t unread = db_.changeCacheFlags(folder->id, uids, flagsFromNames(add), flagsFromNames(remove));
        if (unread != 0) db_.adjustCacheFolderCounts(folder->id, 0, unread, true);
    } catch (const std::exception& e) {
        std::cerr << "[cache] kunde inte uppdatera flaggor: " << e.what() << "\n";
    }
}

void MailCache::messagesRemoved(long long accountId, const std::string& folderPath, const std::vector<std::uint32_t>& uids,
                                const std::string& target) {
    try {
        auto folder = db_.getCacheFolder(accountId, folderPath);
        if (!folder) return;
        CacheRemoved removed = db_.removeCacheMessages(folder->id, uids);
        db_.adjustCacheFolderCounts(folder->id, -removed.count, -removed.unread, true);
        if (!target.empty()) {
            if (auto targetFolder = db_.getCacheFolder(accountId, target)) {
                // Meddelandena får nya UID:n i målmappen; de hämtas vid nästa avstämning.
                db_.adjustCacheFolderCounts(targetFolder->id, removed.count, removed.unread, false);
                db_.markCacheFolderDirty(accountId, target, nowMs());
            }
        }
        kick(accountId);
    } catch (const std::exception& e) {
        std::cerr << "[cache] kunde inte uppdatera efter flytt: " << e.what() << "\n";
    }
}

void MailCache::folderTouched(long long accountId, const std::string& folder) {
    try {
        db_.markCacheFolderDirty(accountId, folder, nowMs());
        kick(accountId);
    } catch (const std::exception& e) {
        std::cerr << "[cache] " << e.what() << "\n";
    }
}

void MailCache::foldersChanged(long long accountId) {
    {
        std::lock_guard lock(mutex_);
        auto& st = states_[accountId];
        st.lastFolderListAt = 0;
        ++st.foldersVersion;
    }
    kick(accountId);
}

void MailCache::folderRenamed(long long accountId, const std::string& from, const std::string& to) {
    try {
        db_.renameCacheFolder(accountId, from, to);
    } catch (const std::exception& e) {
        std::cerr << "[cache] " << e.what() << "\n";
    }
    foldersChanged(accountId);
}

void MailCache::folderDeleted(long long accountId, const std::string& path) {
    try {
        db_.deleteCacheFolder(accountId, path);
    } catch (const std::exception& e) {
        std::cerr << "[cache] " << e.what() << "\n";
    }
    foldersChanged(accountId);
}

void MailCache::accountChanged(long long accountId) {
    {
        std::lock_guard lock(mutex_);
        auto& st = states_[accountId];
        st.reconnect = true;
        st.failures = 0;
    }
    kick(accountId);
}

void MailCache::kick(long long accountId) {
    {
        std::lock_guard lock(mutex_);
        states_[accountId].kicked = true;
    }
    wake_.notify_all();
}

void MailCache::markSeenLater(long long accountId, const MessageRef& ref) {
    {
        std::lock_guard lock(mutex_);
        if (deferred_.size() > 1000) return;  // Mejlservern hinner inte med; nästa avstämning rättar flaggan.
        deferred_.push_back([this, accountId, ref]() { mail_.setFlags(accountId, ref.folder, {ref.uid}, {"seen"}, {}); });
    }
    wake_.notify_all();
}

void MailCache::deferredWorker() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !deferred_.empty(); });
            if (stopping_) return;
            task = std::move(deferred_.front());
            deferred_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            // T.ex. när mejlservern inte går att nå. Cachens flagga rättas vid nästa avstämning.
            std::cerr << "[cache] uppskjuten ändring misslyckades: " << e.what() << "\n";
        }
    }
}

// --- Status -------------------------------------------------------------------------

json MailCache::stateJson(const Account& account) {
    AccountState st;
    {
        std::lock_guard lock(mutex_);
        auto it = states_.find(account.id);
        if (it != states_.end()) st = it->second;
    }
    CacheProgress p = db_.cacheProgress(account.id);
    std::string state = st.offline ? "offline" : st.syncing ? "syncing" : st.lastSyncAt == 0 ? "waiting" : "idle";
    // Första synken pågår tills alla mappars rubriker och all text är hämtad.
    bool complete = p.folders > 0 && p.foldersComplete == p.folders && p.bodies == p.messages;
    return {{"accountId", account.id},
            {"state", state},
            {"error", st.error},
            {"lastSyncAt", st.lastSyncAt},
            {"complete", complete},
            {"folders", p.folders},
            {"foldersComplete", p.foldersComplete},
            {"messages", p.messages},
            {"bodies", p.bodies}};
}

json MailCache::status(const std::vector<Account>& accounts) {
    json out = json::array();
    for (const auto& a : accounts) out.push_back(stateJson(a));
    return out;
}

json MailCache::syncNow(const Account& account, int waitSeconds) {
    std::uint64_t before;
    {
        std::lock_guard lock(mutex_);
        auto& st = states_[account.id];
        before = st.generation;
        st.kicked = true;
    }
    wake_.notify_all();
    {
        std::unique_lock lock(mutex_);
        // Väntar på ett pass som började efter anropet; ett pass som redan pågick kan ha missat det senaste.
        wake_.wait_for(lock, std::chrono::seconds(waitSeconds), [&] {
            const auto& st = states_[account.id];
            return stopping_ || (!st.kicked && !st.syncing && st.generation != before);
        });
    }
    return stateJson(account);
}

// --- Synk -----------------------------------------------------------------------------

void MailCache::worker(int index) {
    std::unique_ptr<Database> wdb;
    std::map<long long, std::unique_ptr<ImapClient>> clients;
    std::int64_t lastPrune = 0;

    auto stopping = [&] {
        std::lock_guard lock(mutex_);
        return stopping_;
    };

    while (!stopping()) {
        bool backlog = false;
        try {
            if (!wdb) wdb = std::make_unique<Database>(config_, false);
            if (index == 0 && now() - lastPrune > kTombstonePrune) {
                wdb->pruneCacheTombstones(now() - kTombstoneKeep);
                lastPrune = now();
            }
            std::set<long long> present;
            for (const auto& account : wdb->listAllAccounts()) {
                if (account.id % kWorkers != index) continue;
                present.insert(account.id);
                if (stopping()) break;
                {
                    std::lock_guard lock(mutex_);
                    auto& st = states_[account.id];
                    if (st.reconnect) {
                        clients.erase(account.id);
                        st.reconnect = false;
                    }
                    if (!st.kicked && now() < st.nextPassAt) continue;
                    st.kicked = false;
                    st.syncing = true;
                }

                bool more = false;
                std::string error;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    auto& client = clients[account.id];
                    bool reused = client && client->connected();
                    try {
                        if (!reused) client = mail_.connectImap(account);
                        more = syncAccount(*wdb, *client, account);
                        error.clear();
                        break;
                    } catch (const NetError& e) {
                        error = e.what();
                        clients.erase(account.id);
                        // Mejlservern kan ha stängt en vilande anslutning; försök en gång till med en ny.
                        if (!reused) break;
                    } catch (const std::exception& e) {
                        error = e.what();
                        clients.erase(account.id);
                        break;
                    }
                }

                {
                    std::lock_guard lock(mutex_);
                    auto& st = states_[account.id];
                    st.syncing = false;
                    ++st.generation;
                    if (error.empty()) {
                        if (st.offline) std::cout << "[cache] konto " << account.id << ": mejlservern svarar igen" << std::endl;
                        st.offline = false;
                        st.error.clear();
                        st.failures = 0;
                        st.lastSyncAt = now();
                        st.nextPassAt = more ? 0 : now() + kPassInterval;
                    } else {
                        if (!st.offline) std::cerr << "[cache] konto " << account.id << ": " << error << "\n";
                        st.offline = true;
                        st.error = error;
                        ++st.failures;
                        st.nextPassAt = now() + std::min<std::int64_t>(600, 30ll << std::min(st.failures - 1, 5));
                    }
                }
                wake_.notify_all();
                backlog = backlog || more;
            }
            for (auto it = clients.begin(); it != clients.end();) {
                it = present.count(it->first) ? std::next(it) : clients.erase(it);
            }
        } catch (const std::exception& e) {
            std::cerr << "[cache] synktråd " << index << ": " << e.what() << "\n";
            wdb.reset();
        }

        std::unique_lock lock(mutex_);
        auto kicked = [&] {
            for (const auto& [id, st] : states_) {
                if (st.kicked && id % kWorkers == index) return true;
            }
            return false;
        };
        wake_.wait_for(lock, backlog ? std::chrono::milliseconds(200) : std::chrono::milliseconds(5000),
                       [&] { return stopping_ || kicked(); });
    }
}

bool MailCache::syncAccount(Database& wdb, ImapClient& client, const Account& account) {
    std::int64_t startedAt = nowMs();
    std::uint64_t foldersVersion;
    std::int64_t prevLastSyncAt;
    {
        std::lock_guard lock(mutex_);
        foldersVersion = states_[account.id].foldersVersion;
        prevLastSyncAt = states_[account.id].lastSyncAt;
    }
    std::int64_t freshSince = prevLastSyncAt == 0 ? now() - kFreshAfterStart
                                                  : std::max(prevLastSyncAt - kFreshMargin, now() - kFreshMax);

    std::vector<CacheFolder> listed;
    for (const auto& f : client.listFolders(true)) {
        if (f.attributes.count("\\nonexistent")) continue;
        CacheFolder c;
        c.accountId = account.id;
        c.path = f.path;
        c.delimiter = f.delimiter;
        c.specialUse = specialUseOf(f);
        c.selectable = !f.attributes.count("\\noselect");
        c.position = static_cast<int>(listed.size());
        c.total = f.total;
        c.unread = f.unread;
        c.uidValidity = f.uidValidity;
        c.uidNext = f.uidNext;
        listed.push_back(std::move(c));
    }
    wdb.saveCacheFolderList(account.id, listed);
    {
        std::lock_guard lock(mutex_);
        auto& st = states_[account.id];
        // Har mapparna ändrats via appen medan listan hämtades kan den redan vara inaktuell; hämta den igen.
        if (st.foldersVersion == foldersVersion) st.lastFolderListAt = now();
        else st.kicked = true;
    }

    auto folders = wdb.listCacheFolders(account.id);
    std::stable_sort(folders.begin(), folders.end(),
                     [](const CacheFolder& a, const CacheFolder& b) { return folderPriority(a) < folderPriority(b); });

    Budget budget{kHeaderBudget, kBodyBudget, kBodyBytesBudget};
    bool more = false;
    auto stopping = [&] {
        std::lock_guard lock(mutex_);
        return stopping_;
    };

    for (auto& f : folders) {
        if (!cacheable(f)) {
            // En mapp som cachades innan den kändes igen som virtuell: ta bort kopiorna, som annars blir inaktuella.
            if (f.headersComplete || f.syncedTotal != -1 || f.syncedUidNext != 0) wdb.purgeCacheMessages(f.id);
            continue;
        }
        if (stopping()) return false;
        if (f.syncedUidValidity != 0 && f.syncedUidValidity != f.uidValidity) {
            // Mejlservern har numrerat om mappen; cachade UID:n pekar inte längre på samma meddelanden.
            std::cout << "[cache] konto " << account.id << ": " << f.path << " har numrerats om, hämtas på nytt" << std::endl;
            wdb.purgeCacheMessages(f.id);
            rawCache_.purgeAccount(account.id);
            f.headersComplete = false;
            f.syncedTotal = -1;
        }
        std::int64_t interval = f.specialUse == "inbox" ? kInboxReconcile : kOtherReconcile;
        bool needed = f.dirtyAt > 0 || !f.headersComplete || f.uidNext != f.syncedUidNext || f.total != f.syncedTotal ||
                      f.unread != f.syncedUnread || now() - f.reconciledAt > interval;
        if (!needed) continue;
        if (budget.headers <= 0) {
            more = true;
            continue;
        }
        try {
            more = reconcileFolder(wdb, client, account, f, budget, startedAt, freshSince) || more;
        } catch (const MailServerError& e) {
            // T.ex. en mapp som togs bort eller bytte namn under tiden. Kontot i övrigt fungerar.
            std::cerr << "[cache] konto " << account.id << ", " << f.path << ": " << e.what() << "\n";
        }
    }

    for (const auto& f : folders) {
        if (!cacheable(f)) continue;
        if (stopping()) return false;
        if (budget.bodies <= 0 || budget.bodyBytes <= 0) {
            more = true;
            break;
        }
        try {
            more = fetchBodies(wdb, client, f, budget) || more;
        } catch (const MailServerError& e) {
            std::cerr << "[cache] konto " << account.id << ", " << f.path << ": " << e.what() << "\n";
        }
    }
    return more;
}

// Stämmer av mappens innehåll mot mejlservern: nya meddelanden, borttagna meddelanden och ändrade flaggor.
bool MailCache::reconcileFolder(Database& wdb, ImapClient& client, const Account& account, CacheFolder& f, Budget& budget,
                                std::int64_t startedAt, std::int64_t freshSince) {
    std::uint32_t exists = client.examine(f.path);
    std::int64_t interval = f.specialUse == "inbox" ? kInboxReconcile : kOtherReconcile;
    bool full = !f.headersComplete || now() - f.reconciledAt > interval;
    std::vector<std::uint32_t> missing;

    if (!full) {
        // Vanligaste fallet är ny post: hämta bara det som kommit till, och kontrollera sedan att antalen stämmer.
        auto counts = wdb.cacheCounts(f.id);
        if (exists > 0 && f.uidNext > counts.maxUid + 1) {
            for (const auto& m : client.uidFetch(std::to_string(counts.maxUid + 1) + ":*", "(UID FLAGS)")) {
                if (m.uid > counts.maxUid) missing.push_back(m.uid);
            }
        }
        std::int64_t expected = counts.count + static_cast<std::int64_t>(missing.size());
        if (expected != static_cast<std::int64_t>(exists)) full = true;
    }

    if (full) {
        missing.clear();
        std::map<std::uint32_t, CacheFlags> server;
        if (exists > 0) {
            for (const auto& m : client.uidFetch("1:*", "(UID FLAGS)")) server[m.uid] = flagsOf(m);
        }
        auto cached = wdb.cacheMessageFlags(f.id);
        std::vector<std::uint32_t> gone;
        wdb.begin();
        try {
            for (const auto& [uid, flags] : cached) {
                auto it = server.find(uid);
                if (it == server.end()) gone.push_back(uid);
                else if (!(it->second == flags)) wdb.setCacheMessageFlags(f.id, uid, it->second);
            }
            for (std::size_t i = 0; i < gone.size(); i += 500) {
                wdb.removeCacheMessages(f.id, {gone.begin() + static_cast<long>(i),
                                               gone.begin() + static_cast<long>(std::min(gone.size(), i + 500))});
            }
            wdb.commit();
        } catch (...) {
            wdb.rollback();
            throw;
        }
        for (const auto& [uid, flags] : server) {
            if (!cached.count(uid)) missing.push_back(uid);
        }
    }

    bool complete = fetchHeaders(wdb, client, account, f, std::move(missing), budget, freshSince);

    if (!full && complete) {
        // Olästa som inte stämmer betyder att något lästs från en annan klient; då behövs en full avstämning nästa pass.
        auto counts = wdb.cacheCounts(f.id);
        if (counts.unread != f.unread) f.reconciledAt = 0;
    }
    f.headersComplete = complete;
    f.syncedUidValidity = f.uidValidity;
    if (complete) {
        f.syncedUidNext = f.uidNext;
        f.syncedTotal = f.total;
        f.syncedUnread = f.reconciledAt == 0 && !full ? -1 : f.unread;
        if (full) f.reconciledAt = now();
    }
    wdb.saveCacheFolderSync(f, startedAt);
    return !complete || (f.reconciledAt == 0 && !full);
}

// Hämtar rubriker för meddelanden som saknas i cachen, nyast först. Returnerar false om budgeten tog slut.
bool MailCache::fetchHeaders(Database& wdb, ImapClient& client, const Account& account, const CacheFolder& f,
                             std::vector<std::uint32_t> missing, Budget& budget, std::int64_t freshSince) {
    // Nya meddelanden är de med UID från och med uidNext vid förra avstämningen. Vid första synken (eller efter en
    // omnumrering) är syncedUidNext 0, och då är inget nytt för användaren.
    bool watch = push_ && notifiable(f) && f.syncedUidNext > 0;
    std::sort(missing.begin(), missing.end(), std::greater<>());
    for (std::size_t i = 0; i < missing.size(); i += kHeaderChunk) {
        if (budget.headers <= 0) return false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
        }
        std::vector<std::uint32_t> chunk(missing.begin() + static_cast<long>(i),
                                         missing.begin() + static_cast<long>(std::min(missing.size(), i + kHeaderChunk)));
        auto fetched = client.uidFetch(ImapClient::uidSetFrom(chunk), kHeaderFetch);
        std::vector<CacheMessage> rows;
        std::vector<NewMail> fresh;
        rows.reserve(fetched.size());
        for (const auto& m : fetched) {
            auto h = mime::parseHeaders(m.body, m.internalDate);
            CacheMessage row;
            row.uid = m.uid;
            row.subject = textutil::utf8Clean(h.subject);
            row.sender = dump(addressesJson(h.from));
            row.recipients = dump(addressesJson(h.to));
            row.date = h.date;
            row.size = static_cast<std::int64_t>(m.size);
            row.flags = flagsOf(m);
            row.hasAttachments = h.hasAttachments;
            if (watch && m.uid >= f.syncedUidNext && !row.flags.seen) {
                // INTERNALDATE följer med vid flytt, så ett gammalt mejl som flyttas hit räknas inte som nytt.
                std::int64_t arrived = mime::parseDate(m.internalDate);
                if (arrived == 0) arrived = h.date;
                if (arrived == 0 || arrived >= freshSince) {
                    NewMail n;
                    n.userId = account.userId;
                    n.accountId = account.id;
                    n.accountName = account.name;
                    n.folder = f.path;
                    n.specialUse = f.specialUse;
                    n.uid = m.uid;
                    if (!h.from.empty()) {
                        n.fromName = textutil::utf8Clean(h.from.front().name);
                        n.fromEmail = h.from.front().email;
                    }
                    n.subject = row.subject;
                    n.date = h.date;
                    n.messageId = h.messageId;
                    fresh.push_back(std::move(n));
                }
            }
            rows.push_back(std::move(row));
        }
        wdb.begin();
        try {
            // Hela omgången får ett ändringsnummer, så att appen hämtar den som en enhet.
            wdb.upsertCacheMessages(f.id, rows);
            wdb.commit();
        } catch (...) {
            wdb.rollback();
            throw;
        }
        // Först när raderna är sparade, så att appen hittar meddelandet när den hämtar ändringarna efter notisen.
        if (!fresh.empty()) push_->enqueue(std::move(fresh));
        budget.headers -= static_cast<int>(chunk.size());
    }
    return true;
}

// Hämtar text för meddelanden som saknar den, nyast först. Returnerar true om det finns fler kvar.
bool MailCache::fetchBodies(Database& wdb, ImapClient& client, const CacheFolder& f, Budget& budget) {
    auto pending = wdb.cacheMessagesWithoutBody(f.id, budget.bodies);
    if (pending.empty()) return false;
    client.examine(f.path);

    auto store = [&](const CacheMessage& m, const std::string& raw, bool truncated) {
        try {
            auto msg = mime::parseMessage(raw, false);
            CacheBody body;
            std::string searchText;
            if (!buildBody(msg, body, searchText)) {
                wdb.setCacheBodyState(m.id, kBodySkipped);
                return;
            }
            wdb.saveCacheBody(m.id, truncated ? kBodyPartial : kBodyCached, body, searchText);
        } catch (const std::exception& e) {
            std::cerr << "[cache] " << f.path << " uid " << m.uid << ": " << e.what() << "\n";
            wdb.setCacheBodyState(m.id, kBodySkipped);
        }
    };

    std::size_t i = 0;
    while (i < pending.size() && budget.bodies > 0 && budget.bodyBytes > 0) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
        }
        std::map<std::uint32_t, const CacheMessage*> batch;
        bool truncated = pending[i].size > kFullFetchMax;
        std::string items = "(UID BODY.PEEK[])";
        if (truncated) {
            items = "(UID BODY.PEEK[]<0." + std::to_string(kFullFetchMax) + ">)";
            batch[pending[i].uid] = &pending[i];
            ++i;
        } else {
            // Små meddelanden hämtas flera åt gången för att slippa en rundtur per meddelande.
            std::int64_t bytes = 0;
            while (i < pending.size() && pending[i].size <= kFullFetchMax && static_cast<int>(batch.size()) < kBodyBatchCount &&
                   (batch.empty() || bytes + pending[i].size <= kBodyBatchBytes)) {
                bytes += pending[i].size;
                batch[pending[i].uid] = &pending[i];
                ++i;
            }
        }

        std::vector<std::uint32_t> uids;
        for (const auto& [uid, m] : batch) uids.push_back(uid);
        auto fetched = client.uidFetch(ImapClient::uidSetFrom(uids), items);
        wdb.begin();
        try {
            for (const auto& m : fetched) {
                auto it = batch.find(m.uid);
                if (it == batch.end()) continue;
                store(*it->second, m.body, truncated);
                budget.bodyBytes -= static_cast<std::int64_t>(m.body.size());
                batch.erase(it);
            }
            // Meddelanden som försvunnit sedan rubrikerna hämtades; de tas bort ur cachen vid nästa avstämning.
            for (const auto& [uid, m] : batch) wdb.setCacheBodyState(m->id, kBodySkipped);
            wdb.commit();
        } catch (...) {
            wdb.rollback();
            throw;
        }
        budget.bodies -= static_cast<int>(uids.size());
    }
    return i < pending.size() || static_cast<int>(pending.size()) >= kBodyBudget;
}

}  // namespace brevduva
