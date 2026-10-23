#include "backup.hpp"

#include "crypto.hpp"
#include "errors.hpp"
#include "gdrive.hpp"
#include "mime.hpp"
#include "zip_writer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <set>
#include <unordered_set>

namespace brevduva {

namespace {

constexpr int kWorkers = 2;
constexpr auto kWakeInterval = std::chrono::seconds(30);
constexpr auto kReportInterval = std::chrono::seconds(2);
constexpr std::int64_t kZipKeepSeconds = 7 * 24 * 3600;
constexpr std::int64_t kHistorySeconds = 180 * 24 * 3600;
constexpr std::uint64_t kBatchBytes = 16 * 1024 * 1024;
constexpr std::size_t kBatchMessages = 100;
constexpr std::size_t kMaxWarnings = 50;
constexpr int kNetAttempts = 4;

const char* kSourceFetch = "(UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (MESSAGE-ID SUBJECT)])";
const char* kTargetFetch = "(UID INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])";

// Kastas när en körning avbryts av användaren eller när servern stängs av.
struct Cancelled {};

std::int64_t now() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& s, const std::string& delimiter) {
    if (delimiter.empty()) return {s};
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find(delimiter, start);
        if (pos == std::string::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + delimiter.size();
    }
}

std::string join(const std::vector<std::string>& parts, const std::string& delimiter) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? delimiter : "") + parts[i];
    return out;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

// Värdet av ett headerfält i ett headerblock, med vikta rader sammanslagna. Tomt om fältet saknas.
std::string headerValue(const std::string& block, const std::string& name) {
    std::string haystack = lower(block);
    std::string needle = lower(name) + ":";
    for (std::size_t pos = 0; (pos = haystack.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        if (pos != 0 && haystack[pos - 1] != '\n') continue;
        std::size_t start = pos + needle.size();
        std::size_t end = start;
        while (true) {
            end = block.find('\n', end);
            if (end == std::string::npos) {
                end = block.size();
                break;
            }
            if (end + 1 < block.size() && (block[end + 1] == ' ' || block[end + 1] == '\t')) {
                ++end;
                continue;
            }
            break;
        }
        std::string value = block.substr(start, end - start);
        std::erase_if(value, [](char c) { return c == '\r' || c == '\n'; });
        return trim(value);
    }
    return {};
}

// Känner igen samma meddelande hos målet: Message-ID, annars mottagningstid och storlek.
std::string messageKey(const FetchedMessage& m) {
    std::string id = headerValue(m.body, "Message-ID");
    std::size_t open = id.find('<');
    std::size_t close = id.find('>', open == std::string::npos ? 0 : open);
    if (open != std::string::npos && close != std::string::npos) id = id.substr(open + 1, close - open - 1);
    id = trim(id);
    if (!id.empty()) return "id:" + lower(id);
    return "date:" + m.internalDate + "|" + std::to_string(m.size);
}

// Systemflaggor som följer med till målet. Nyckelord hoppas över eftersom alla servrar inte tillåter dem.
std::string appendFlags(const std::set<std::string>& flags) {
    static const std::vector<std::pair<std::string, std::string>> known = {
        {"\\seen", "\\Seen"}, {"\\answered", "\\Answered"}, {"\\flagged", "\\Flagged"}, {"\\draft", "\\Draft"}};
    std::string out;
    for (const auto& [name, flag] : known) {
        if (flags.count(name)) out += (out.empty() ? "" : " ") + flag;
    }
    return out;
}

// Ett namn som fungerar som fil- eller mappnamn i Windows, macOS och Linux.
std::string safeName(const std::string& in, std::size_t maxBytes) {
    std::string out;
    for (unsigned char c : in) {
        bool bad = c < 0x20 || c == 0x7f || std::strchr("<>:\"/\\|?*", c) != nullptr;
        out += bad ? '_' : static_cast<char>(c);
    }
    if (out.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;  // Hela UTF-8-tecken.
        out.resize(cut);
    }
    out = trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) return "_";
    static const std::set<std::string> reserved = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                                   "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                   "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (reserved.count(lower(out.substr(0, out.find('.'))))) out = "_" + out;
    return out;
}

std::string dateStamp(std::int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H%M%S", &tm);
    return buf;
}

struct Progress {
    std::string phase = "connecting";  // connecting, messages, finishing, uploading, cleanup, done
    std::string folder;
    std::size_t foldersDone = 0;
    std::size_t foldersTotal = 0;
    std::uint64_t messagesDone = 0;
    std::uint64_t messagesTotal = 0;
    std::uint64_t copied = 0;
    std::uint64_t skipped = 0;
    std::uint64_t bytes = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t uploadTotal = 0;
    std::vector<std::string> warnings;
    std::size_t warningCount = 0;

    json toJson() const {
        return {
            {"phase", phase},
            {"folder", folder},
            {"foldersDone", foldersDone},
            {"foldersTotal", foldersTotal},
            {"messagesDone", messagesDone},
            {"messagesTotal", messagesTotal},
            {"copied", copied},
            {"skipped", skipped},
            {"bytes", bytes},
            {"uploaded", uploaded},
            {"uploadTotal", uploadTotal},
            {"warnings", warnings},
            {"warningCount", warningCount},
        };
    }
};

enum class WriteResult { Stored, Skipped, Failed };

// Ett mål för meddelanden som läses från källkontot, mapp för mapp.
class BackupSink {
public:
    virtual ~BackupSink() = default;
    // Returnerar false om mappen inte kan tas emot; den hoppas då över.
    virtual bool beginFolder(const FolderInfo& folder, const std::vector<FetchedMessage>& messages) = 0;
    // false för meddelanden som redan finns hos målet, så att de inte behöver hämtas.
    virtual bool wants(const FetchedMessage&) { return true; }
    virtual WriteResult write(const FetchedMessage& meta, const std::string& raw) = 0;
    virtual void endFolder() {}
};

}  // namespace

// Utför en körning. Skriver resultatet i run.
class BackupRunner {
public:
    BackupRunner(BackupService& service, BackupRun& run, std::atomic<bool>& cancelled)
        : s_(service), run_(run), cancelled_(cancelled) {}

    void execute();

    void checkCancel() {
        if (cancelled_) throw Cancelled{};
    }

    void warn(const std::string& text) {
        std::cerr << "[backup] run " << run_.id << ": " << text << "\n";
        ++progress_.warningCount;
        if (progress_.warnings.size() < kMaxWarnings) progress_.warnings.push_back(text);
    }

    void report(bool force) {
        auto t = std::chrono::steady_clock::now();
        if (!force && t - lastReport_ < kReportInterval) return;
        lastReport_ = t;
        try {
            s_.db_.setBackupRunProgress(run_.id, progress_.toJson().dump(-1, ' ', false, json::error_handler_t::replace));
        } catch (const std::exception& e) {
            std::cerr << "[backup] could not save progress: " << e.what() << "\n";
        }
    }

    // Kör fn. Vid nätverksfel väntas en stund, reconnect anropas och fn körs igen.
    template <typename F>
    auto retry(F&& fn, const std::function<void()>& reconnect) -> decltype(fn()) {
        for (int attempt = 1;; ++attempt) {
            try {
                if (attempt > 1) reconnect();
                return fn();
            } catch (const NetError& e) {
                if (attempt >= kNetAttempts) throw;
                std::cerr << "[backup] run " << run_.id << ": " << e.what() << "; retrying\n";
                for (int i = 0; i < attempt * 5; ++i) {
                    checkCancel();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
    }

    MailService& mail() { return s_.mail_; }

private:
    Account ownedAccount(long long id, const std::string& role);
    void copy(const Account& source, const std::set<std::string>& excluded, BackupSink& sink);
    std::uint64_t writeZip(const Account& source, const std::set<std::string>& excluded, const std::string& path);
    void toZip(const Account& source, const std::set<std::string>& excluded);
    void toImap(const Account& source, const std::set<std::string>& excluded, const json& destination);
    void toDrive(const Account& source, const std::set<std::string>& excluded, const json& destination);
    std::string tempFile(const std::string& path);

    BackupService& s_;
    BackupRun& run_;
    std::atomic<bool>& cancelled_;
    Progress progress_;
    std::chrono::steady_clock::time_point lastReport_{};
    std::string tempPath_;  // Tas bort om körningen misslyckas.
};

namespace {

// Skriver varje meddelande som en .eml-fil i en mappstruktur som speglar kontot, plus en
// innehållsförteckning (brevduva-backup.json) med flaggor och mottagningstider.
class ZipSink : public BackupSink {
public:
    explicit ZipSink(const std::string& path) : zip_(path) {}

    bool beginFolder(const FolderInfo& folder, const std::vector<FetchedMessage>& messages) override {
        std::vector<std::string> parts;
        for (const auto& part : split(folder.path, folder.delimiter)) parts.push_back(safeName(part, 100));
        std::string last = parts.back();
        // Mappar som bara skiljer sig i skiftläge eller i otillåtna tecken får ett löpnummer.
        for (int n = 2; usedDirs_.count(lower(join(parts, "/"))); ++n) parts.back() = last + " (" + std::to_string(n) + ")";
        dir_ = join(parts, "/");
        usedDirs_.insert(lower(dir_));
        folder_ = folder;
        messages_.clear();
        if (messages.empty()) zip_.add(dir_ + "/", "", now());
        return true;
    }

    WriteResult write(const FetchedMessage& m, const std::string& raw) override {
        auto headers = mime::parseHeaders(m.body, m.internalDate);
        char uid[16];
        std::snprintf(uid, sizeof(uid), "%06u", m.uid);
        std::string subject = headers.subject.empty() ? "(no subject)" : headers.subject;
        std::string name = dir_ + "/" + uid + " " + safeName(subject, 80) + ".eml";
        zip_.add(name, raw, headers.date > 0 ? static_cast<std::time_t>(headers.date) : std::time(nullptr));

        json flags = json::array();
        for (const auto& f : m.flags) {
            if (f != "\\recent") flags.push_back(f);  // \Recent gäller bara den aktuella sessionen.
        }
        json entry = {{"file", name}, {"uid", m.uid}, {"flags", flags}, {"internalDate", m.internalDate},
                      {"size", raw.size()}, {"messageId", headerValue(m.body, "Message-ID")}};
        messages_ += (messages_.empty() ? "" : ",") + entry.dump(-1, ' ', false, json::error_handler_t::replace);
        return WriteResult::Stored;
    }

    void endFolder() override {
        json info = {{"path", folder_.path}, {"delimiter", folder_.delimiter}, {"specialUse", specialUseOf(folder_)},
                     {"directory", dir_}};
        std::string dumped = info.dump(-1, ' ', false, json::error_handler_t::replace);
        dumped.pop_back();  // Avslutande }, så att meddelandelistan kan läggas till utan att bygga om JSON.
        folders_ += (folders_.empty() ? "" : ",") + dumped + ",\"messages\":[" + messages_ + "]}";
        messages_.clear();
    }

    std::uint64_t finish(const json& header) {
        std::string head = header.dump(-1, ' ', false, json::error_handler_t::replace);
        head.pop_back();
        zip_.add("brevduva-backup.json", head + ",\"folders\":[" + folders_ + "]}", std::time(nullptr));
        zip_.finish();
        return zip_.bytesWritten();
    }

private:
    ZipWriter zip_;
    std::set<std::string> usedDirs_;
    std::string dir_;
    FolderInfo folder_;
    std::string messages_;  // JSON-poster för aktuell mapp
    std::string folders_;   // JSON-poster för klara mappar
};

// Kopierar meddelanden till ett annat IMAP-konto. Meddelanden som redan finns i målmappen hoppas över,
// så att en schemalagd backup bara kopierar det som är nytt.
class ImapSink : public BackupSink {
public:
    ImapSink(BackupRunner& runner, Account target, const std::string& folderPrefix)
        : r_(runner), target_(std::move(target)) {
        for (const auto& part : split(folderPrefix, "/")) {
            if (!trim(part).empty()) prefix_.push_back(trim(part));
        }
    }

    void open() {
        auto folders = r_.retry(
            [&] {
                connect();
                return client_->listFolders(false);
            },
            [] {});
        for (const auto& f : folders) {
            existing_.insert(f.path);
            std::string use = specialUseOf(f);
            if (!use.empty() && !special_.count(use)) special_[use] = f.path;
            if (f.path == "INBOX" && !f.delimiter.empty()) delimiter_ = f.delimiter;
        }
        for (const auto& f : folders) {
            if (delimiter_.empty() && !f.delimiter.empty()) delimiter_ = f.delimiter;
        }
        if (delimiter_.empty()) delimiter_ = "/";
    }

    void close() {
        if (client_) client_->logout();
    }

    bool beginFolder(const FolderInfo& folder, const std::vector<FetchedMessage>&) override {
        path_ = targetPath(folder);
        keys_.clear();
        try {
            ensureFolder(path_);
            auto existing = r_.retry(
                [&] {
                    std::uint32_t count = client_->examine(path_);
                    return count ? client_->uidFetch("1:*", kTargetFetch) : std::vector<FetchedMessage>{};
                },
                [&] { connect(); });
            for (const auto& m : existing) keys_.insert(messageKey(m));
            return true;
        } catch (const MailServerError& e) {
            r_.warn("Folder " + folder.path + " was not copied: " + e.what());
            return false;
        }
    }

    bool wants(const FetchedMessage& m) override { return !keys_.count(messageKey(m)); }

    WriteResult write(const FetchedMessage& m, const std::string& raw) override {
        std::string key = messageKey(m);
        if (keys_.count(key)) return WriteResult::Skipped;  // Dubblett i källmappen.
        try {
            r_.retry([&] { return client_->append(path_, appendFlags(m.flags), raw, m.internalDate); },
                     [&] { connect(); });
            keys_.insert(key);
            return WriteResult::Stored;
        } catch (const MailServerError& e) {
            std::string subject = mime::parseHeaders(m.body, m.internalDate).subject;
            r_.warn("Message \"" + subject + "\" could not be saved in " + path_ + ": " + e.what());
            return WriteResult::Failed;
        }
    }

private:
    void connect() {
        if (client_) client_->logout();
        client_ = r_.mail().connectImap(target_);
    }

    // Utan målmapp hamnar inkorgen i målets inkorg och specialmappar i motsvarande mappar hos målet.
    std::string targetPath(const FolderInfo& folder) {
        std::string use = specialUseOf(folder);
        if (prefix_.empty()) {
            if (use == "inbox") return "INBOX";
            static const std::set<std::string> mapped = {"sent", "drafts", "trash", "junk", "archive"};
            auto it = special_.find(use);
            if (mapped.count(use) && it != special_.end()) return it->second;
        }
        // Delar som innehåller målets avgränsare skulle annars bli en extra mappnivå.
        std::vector<std::string> parts;
        for (const auto& part : prefix_) parts.push_back(replaceAll(part, delimiter_, "_"));
        for (const auto& part : split(folder.path, folder.delimiter)) parts.push_back(replaceAll(part, delimiter_, "_"));
        return join(parts, delimiter_);
    }

    void ensureFolder(const std::string& path) {
        if (existing_.count(path)) return;
        auto parts = split(path, delimiter_);
        std::string current;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            current += (i ? delimiter_ : "") + parts[i];
            if (existing_.count(current)) continue;
            try {
                r_.retry(
                    [&] {
                        client_->createFolder(current);
                        return 0;
                    },
                    [&] { connect(); });
            } catch (const MailServerError& e) {
                // Överordnade mappar skapas automatiskt av en del servrar, och mappen kan redan finnas.
                bool last = i + 1 == parts.size();
                if (last && lower(e.what()).find("exist") == std::string::npos) throw;
            }
            existing_.insert(current);
        }
    }

    BackupRunner& r_;
    Account target_;
    std::vector<std::string> prefix_;
    std::unique_ptr<ImapClient> client_;
    std::set<std::string> existing_;
    std::map<std::string, std::string> special_;
    std::string delimiter_;
    std::string path_;
    std::unordered_set<std::string> keys_;
};

json manifestHeader(const Account& source, std::int64_t createdAt) {
    return {{"format", "brevduva-backup"}, {"version", 1},
            {"account", {{"email", source.email}, {"name", source.name}, {"imapHost", source.imapHost}}},
            {"createdAt", createdAt}};
}

}  // namespace

Account BackupRunner::ownedAccount(long long id, const std::string& role) {
    auto account = s_.db_.getAccount(id);
    if (!account || account->userId != run_.userId) throw MailServerError(role + " no longer exists");
    return *account;
}

void BackupRunner::copy(const Account& source, const std::set<std::string>& excluded, BackupSink& sink) {
    std::unique_ptr<ImapClient> client;
    std::string openFolder;
    auto connect = [&]() {
        if (client) client->logout();
        client = s_.mail_.connectImap(source);
        if (!openFolder.empty()) client->examine(openFolder);
    };

    progress_.phase = "connecting";
    report(true);
    auto folders = retry(
        [&] {
            if (!client) connect();
            return client->listFolders(true);
        },
        connect);

    std::vector<FolderInfo> selected;
    for (auto& f : folders) {
        if (f.attributes.count("\\noselect") || f.attributes.count("\\nonexistent") || excluded.count(f.path)) continue;
        progress_.messagesTotal += f.total;
        selected.push_back(std::move(f));
    }
    progress_.foldersTotal = selected.size();
    progress_.phase = "messages";
    report(true);

    for (const auto& folder : selected) {
        checkCancel();
        progress_.folder = folder.path;
        report(false);

        std::vector<FetchedMessage> messages;
        openFolder.clear();
        try {
            messages = retry(
                [&] {
                    std::uint32_t count = client->examine(folder.path);
                    openFolder = folder.path;
                    return count ? client->uidFetch("1:*", kSourceFetch) : std::vector<FetchedMessage>{};
                },
                connect);
        } catch (const MailServerError& e) {
            warn("Folder " + folder.path + " was skipped: " + e.what());
            progress_.messagesDone += folder.total;
            ++progress_.foldersDone;
            continue;
        }
        std::erase_if(messages, [](const FetchedMessage& m) { return m.flags.count("\\deleted") > 0; });
        std::sort(messages.begin(), messages.end(),
                  [](const FetchedMessage& a, const FetchedMessage& b) { return a.uid < b.uid; });
        // Antalet från STATUS kan ha ändrats sedan mapplistan hämtades.
        progress_.messagesTotal = progress_.messagesTotal - std::min<std::uint64_t>(progress_.messagesTotal, folder.total) +
                                  messages.size();

        if (!sink.beginFolder(folder, messages)) {
            progress_.messagesDone += messages.size();
            ++progress_.foldersDone;
            continue;
        }

        std::vector<const FetchedMessage*> todo;
        for (const auto& m : messages) {
            if (sink.wants(m)) {
                todo.push_back(&m);
            } else {
                ++progress_.skipped;
                ++progress_.messagesDone;
            }
        }

        for (std::size_t i = 0; i < todo.size();) {
            checkCancel();
            // Hämta i omgångar som begränsas av antal och storlek, så att minnet räcker även för stora mappar.
            std::vector<std::uint32_t> uids;
            std::uint64_t batchBytes = 0;
            std::size_t end = i;
            while (end < todo.size() && uids.size() < kBatchMessages &&
                   (uids.empty() || batchBytes + todo[end]->size <= kBatchBytes)) {
                uids.push_back(todo[end]->uid);
                batchBytes += todo[end]->size;
                ++end;
            }

            std::map<std::uint32_t, std::string> bodies;
            auto fetch = [&](const std::vector<std::uint32_t>& set) {
                auto fetched = retry([&] { return client->uidFetch(ImapClient::uidSetFrom(set), "(UID BODY.PEEK[])"); },
                                     connect);
                for (auto& m : fetched) bodies[m.uid] = std::move(m.body);
            };
            try {
                fetch(uids);
            } catch (const MailServerError& e) {
                // Hämta ett i taget för att hitta det meddelande som servern inte kan leverera.
                for (auto uid : uids) {
                    try {
                        fetch({uid});
                    } catch (const MailServerError& single) {
                        warn("Message " + std::to_string(uid) + " in " + folder.path + " could not be fetched: " +
                             single.what());
                    }
                }
            }

            for (std::size_t k = i; k < end; ++k) {
                auto it = bodies.find(todo[k]->uid);
                if (it != bodies.end()) {
                    switch (sink.write(*todo[k], it->second)) {
                        case WriteResult::Stored:
                            ++progress_.copied;
                            progress_.bytes += it->second.size();
                            break;
                        case WriteResult::Skipped:
                            ++progress_.skipped;
                            break;
                        case WriteResult::Failed:
                            break;
                    }
                }
                ++progress_.messagesDone;
            }
            report(false);
            i = end;
        }
        sink.endFolder();
        ++progress_.foldersDone;
        report(false);
    }
    progress_.folder.clear();
    client->logout();
}

std::string BackupRunner::tempFile(const std::string& path) {
    tempPath_ = path;
    std::lock_guard lock(s_.mutex_);
    s_.activeFiles_.insert(path);
    return path;
}

std::uint64_t BackupRunner::writeZip(const Account& source, const std::set<std::string>& excluded,
                                     const std::string& path) {
    ZipSink sink(path);
    copy(source, excluded, sink);
    progress_.phase = "finishing";
    report(true);
    return sink.finish(manifestHeader(source, run_.startedAt));
}

void BackupRunner::toZip(const Account& source, const std::set<std::string>& excluded) {
    std::string name = "backup-" + std::to_string(run_.id) + "-" + crypto::toHex(crypto::randomBytes(6)) + ".zip";
    std::string path = tempFile((std::filesystem::path(s_.config_.backupDir) / name).string());
    run_.fileSize = static_cast<long long>(writeZip(source, excluded, path));
    run_.filePath = path;
    run_.expiresAt = now() + kZipKeepSeconds;
    tempPath_.clear();
    run_.message = std::to_string(progress_.copied) + " messages from " + std::to_string(progress_.foldersDone) +
                   " folders. The file can be downloaded for 7 days.";
}

void BackupRunner::toImap(const Account& source, const std::set<std::string>& excluded, const json& destination) {
    Account target = ownedAccount(destination.at("accountId").get<long long>(), "The target account");
    ImapSink sink(*this, target, destination.value("folder", ""));
    progress_.phase = "connecting";
    report(true);
    sink.open();
    copy(source, excluded, sink);
    sink.close();
    run_.message = std::to_string(progress_.copied) + " messages were copied to " + target.email + ", " +
                   std::to_string(progress_.skipped) + " already existed.";
}

void BackupRunner::toDrive(const Account& source, const std::set<std::string>& excluded, const json& destination) {
    auto connection = s_.db_.getDriveConnection(destination.at("connectionId").get<long long>());
    if (!connection || connection->userId != run_.userId) throw MailServerError("The Google Drive connection no longer exists");
    GoogleDrive drive(s_.config_.googleApiUrl, [&](bool forceRefresh) {
        if (forceRefresh) s_.oauth_.forgetDrive(connection->id);
        return s_.oauth_.accessToken(*connection);
    });

    // Kontrollera åtkomsten till Drive innan e-posten hämtas, så att ett fel syns direkt.
    progress_.phase = "connecting";
    report(true);
    std::string folderId = drive.ensureFolder(BackupService::kDriveFolderName, connection->folderId);
    if (folderId != connection->folderId) s_.db_.setDriveFolderId(connection->id, folderId);

    std::string path = tempFile(
        (std::filesystem::path(s_.config_.backupDir) / ("upload-" + std::to_string(run_.id) + ".zip")).string());
    std::uint64_t size = writeZip(source, excluded, path);

    checkCancel();
    progress_.phase = "uploading";
    progress_.uploadTotal = size;
    report(true);
    std::string name = "brevduva-" + safeName(source.email, 100) + "-" + dateStamp(run_.startedAt) + ".zip";
    std::string jobKey = run_.jobId ? std::to_string(run_.jobId) : "manual";
    auto file = drive.upload(path, name, folderId, {{"brevduvaJob", jobKey}, {"brevduvaAccount", source.email}},
                             [&](std::uint64_t sent, std::uint64_t) {
                                 progress_.uploaded = sent;
                                 report(false);
                                 checkCancel();
                                 return true;
                             });
    std::error_code ec;
    std::filesystem::remove(path, ec);
    run_.fileSize = static_cast<long long>(size);

    // Behåll de senaste backuperna för det här schemat och ta bort äldre.
    int removed = 0;
    if (run_.jobId) {
        progress_.phase = "cleanup";
        report(true);
        int keep = std::max(1, destination.value("keep", 7));
        try {
            int kept = 1;  // Den nya filen.
            for (const auto& old : drive.listByProperty(folderId, "brevduvaJob", jobKey)) {
                if (old.id == file.id) continue;
                if (kept < keep) {
                    ++kept;
                    continue;
                }
                drive.remove(old.id);
                ++removed;
            }
        } catch (const std::exception& e) {
            warn(std::string("Older backups in Google Drive could not be deleted: ") + e.what());
        }
    }
    run_.message = std::to_string(progress_.copied) + " messages were saved to Google Drive (" + connection->email +
                   ") as " + name + "." +
                   (removed ? " " + std::to_string(removed) + " older backup" + (removed == 1 ? "" : "s") + " deleted."
                            : "");
}

void BackupRunner::execute() {
    try {
        json config = json::parse(run_.config);
        Account source = ownedAccount(config.at("sourceAccountId").get<long long>(), "The source account");
        std::set<std::string> excluded;
        for (const auto& f : config.value("excludeFolders", json::array())) excluded.insert(f.get<std::string>());
        const json& destination = config.at("destination");
        std::string type = destination.value("type", "");
        if (type == "zip") {
            toZip(source, excluded);
        } else if (type == "imap") {
            toImap(source, excluded, destination);
        } else if (type == "gdrive") {
            toDrive(source, excluded, destination);
        } else {
            throw std::runtime_error("Unknown backup destination: " + type);
        }
        run_.status = "done";
        progress_.phase = "done";
        if (progress_.warningCount) {
            run_.message += " " + std::to_string(progress_.warningCount) + " warning" +
                            (progress_.warningCount == 1 ? "" : "s") + ".";
        }
    } catch (const Cancelled&) {
        std::lock_guard lock(s_.mutex_);
        run_.status = "cancelled";
        run_.message = s_.stopping_ ? "Cancelled because the server was shut down" : "Cancelled";
    } catch (const std::exception& e) {
        run_.status = "failed";
        run_.message = e.what();
    }

    if (!tempPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
    }
    {
        std::lock_guard lock(s_.mutex_);
        s_.activeFiles_.erase(tempPath_);
        s_.activeFiles_.erase(run_.filePath);
    }
    run_.finishedAt = now();
    run_.progress = progress_.toJson().dump(-1, ' ', false, json::error_handler_t::replace);
    s_.db_.finishBackupRun(run_);
}

// --- BackupService ------------------------------------------------------------

BackupService::BackupService(const Config& config, Database& db, MailService& mail, GoogleOAuth& oauth)
    : config_(config), db_(db), mail_(mail), oauth_(oauth) {}

BackupService::~BackupService() {
    stop();
}

void BackupService::start() {
    std::lock_guard lock(mutex_);
    if (started_) return;
    started_ = true;
    db_.failInterruptedBackupRuns(now());
    for (int i = 0; i < kWorkers; ++i) threads_.emplace_back(&BackupService::worker, this);
    threads_.emplace_back(&BackupService::scheduler, this);
}

void BackupService::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopping_) return;
        stopping_ = true;
        for (auto& [id, flag] : running_) *flag = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void BackupService::worker() {
    while (true) {
        std::uint64_t seen;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            seen = generation_;
        }
        std::optional<BackupRun> run;
        try {
            run = db_.claimQueuedBackupRun(now());
        } catch (const std::exception& e) {
            std::cerr << "[backup] could not fetch the next run: " << e.what() << "\n";
        }
        if (run) {
            execute(std::move(*run));
            continue;
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, kWakeInterval, [&] { return stopping_ || generation_ != seen; });
    }
}

void BackupService::execute(BackupRun run) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || cancelRequested_.erase(run.id)) *flag = true;
        running_[run.id] = flag;
    }
    std::cout << "[backup] starting run " << run.id << std::endl;
    BackupRunner runner(*this, run, *flag);
    try {
        runner.execute();
    } catch (const std::exception& e) {
        std::cerr << "[backup] run " << run.id << " could not be finished: " << e.what() << "\n";
    }
    {
        std::lock_guard lock(mutex_);
        running_.erase(run.id);
    }
    std::cout << "[backup] run " << run.id << ": " << run.status << " " << run.message << std::endl;
}

void BackupService::scheduler() {
    std::int64_t lastCleanup = 0;
    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
        }
        std::int64_t t = now();
        try {
            runScheduledJobs(t);
        } catch (const std::exception& e) {
            std::cerr << "[backup] scheduler: " << e.what() << "\n";
        }
        if (t - lastCleanup >= 3600) {
            lastCleanup = t;
            try {
                cleanup(t);
            } catch (const std::exception& e) {
                std::cerr << "[backup] cleanup: " << e.what() << "\n";
            }
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, kWakeInterval, [&] { return stopping_; });
    }
}

void BackupService::runScheduledJobs(std::int64_t t) {
    for (const auto& job : db_.dueBackupJobs(t)) {
        json schedule = json::parse(job.schedule, nullptr, false);
        db_.setBackupJobNextRun(job.id, schedule.is_discarded() ? 0 : nextRunAfter(schedule, t));
        if (db_.hasActiveBackupRun(job.id)) {
            std::cerr << "[backup] " << job.name << " is still running; the scheduled run is skipped\n";
            continue;
        }
        try {
            enqueue(job.userId, job.id, validateConfig(json::parse(job.config), job.userId));
        } catch (const std::exception& e) {
            // Visa felet i historiken, t.ex. när ett konto har tagits bort.
            BackupRun failed;
            failed.userId = job.userId;
            failed.jobId = job.id;
            failed.status = "failed";
            failed.config = job.config;
            failed.message = std::string("The scheduled backup could not start: ") + e.what();
            failed.finishedAt = t;
            db_.createBackupRun(failed);
        }
    }
}

void BackupService::cleanup(std::int64_t t) {
    for (const auto& run : db_.expiredBackupFiles(t)) removeFile(run);

    std::set<std::string> known;
    for (auto& path : db_.backupRunFiles()) known.insert(std::move(path));
    {
        std::lock_guard lock(mutex_);
        known.insert(activeFiles_.begin(), activeFiles_.end());
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(config_.backupDir, ec)) {
        if (!entry.is_regular_file() || known.count(entry.path().string())) continue;
        auto age = std::filesystem::file_time_type::clock::now() - entry.last_write_time();
        if (age < std::chrono::hours(1)) continue;
        std::cout << "[backup] removing leftover file " << entry.path() << std::endl;
        std::filesystem::remove(entry.path(), ec);
    }
    db_.pruneBackupRuns(t - kHistorySeconds);
}

BackupRun BackupService::enqueue(long long userId, long long jobId, const json& config) {
    if (jobId && db_.hasActiveBackupRun(jobId)) throw BadRequest("The backup is already running");
    BackupRun run;
    run.userId = userId;
    run.jobId = jobId;
    run.status = "queued";
    json stored = config;
    if (jobId) {
        // Namnet sparas i körningen, så att historiken går att läsa även om schemat tas bort.
        if (auto job = db_.getBackupJob(jobId)) stored["jobName"] = job->name;
    }
    run.config = stored.dump(-1, ' ', false, json::error_handler_t::replace);
    run.id = db_.createBackupRun(run);
    {
        std::lock_guard lock(mutex_);
        ++generation_;
    }
    wake_.notify_all();
    return *db_.getBackupRun(run.id);
}

void BackupService::cancel(long long runId) {
    if (db_.cancelQueuedBackupRun(runId, now())) return;
    std::lock_guard lock(mutex_);
    auto it = running_.find(runId);
    if (it != running_.end()) {
        *it->second = true;
    } else {
        cancelRequested_.insert(runId);  // Precis hämtad av en arbetstråd men inte startad än.
    }
}

void BackupService::removeFile(const BackupRun& run) {
    if (!run.filePath.empty()) {
        // Ta bara bort filer i backupkatalogen.
        std::filesystem::path path(run.filePath);
        if (path.parent_path() == std::filesystem::path(config_.backupDir)) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    db_.clearBackupRunFile(run.id);
}

std::vector<std::string> BackupService::jobsUsingAccount(long long userId, long long accountId) {
    std::vector<std::string> names;
    for (const auto& job : db_.listBackupJobs(userId)) {
        json c = json::parse(job.config, nullptr, false);
        if (c.is_discarded()) continue;
        const json dest = c.value("destination", json::object());
        if (c.value("sourceAccountId", 0LL) == accountId ||
            (dest.value("type", "") == "imap" && dest.value("accountId", 0LL) == accountId)) {
            names.push_back(job.name);
        }
    }
    return names;
}

std::vector<std::string> BackupService::jobsUsingDrive(long long userId, long long connectionId) {
    std::vector<std::string> names;
    for (const auto& job : db_.listBackupJobs(userId)) {
        json c = json::parse(job.config, nullptr, false);
        if (c.is_discarded()) continue;
        const json dest = c.value("destination", json::object());
        if (dest.value("type", "") == "gdrive" && dest.value("connectionId", 0LL) == connectionId) {
            names.push_back(job.name);
        }
    }
    return names;
}

json BackupService::validateConfig(const json& in, long long userId) {
    if (!in.is_object()) throw BadRequest("Backup settings are missing");
    json out;

    long long sourceId = in.value("sourceAccountId", 0LL);
    auto source = db_.getAccount(sourceId);
    if (!source || source->userId != userId) throw BadRequest("Choose which account to back up");
    out["sourceAccountId"] = source->id;
    out["sourceEmail"] = source->email;

    if (!in.contains("destination") || !in["destination"].is_object()) throw BadRequest("Choose where to save the backup");
    const json& dest = in["destination"];
    std::string type = dest.value("type", "");
    if (type == "zip") {
        out["destination"] = {{"type", "zip"}};
    } else if (type == "imap") {
        long long targetId = dest.value("accountId", 0LL);
        auto target = db_.getAccount(targetId);
        if (!target || target->userId != userId) throw BadRequest("Choose an IMAP account to copy to");
        if (target->id == source->id) throw BadRequest("Choose an account other than the one being backed up");
        std::string folder = trim(dest.value("folder", ""));
        while (!folder.empty() && folder.front() == '/') folder.erase(0, 1);
        while (!folder.empty() && folder.back() == '/') folder.pop_back();
        if (folder.size() > 200) throw BadRequest("The target folder name is too long");
        out["destination"] = {{"type", "imap"}, {"accountId", target->id}, {"email", target->email}, {"folder", folder}};
    } else if (type == "gdrive") {
        long long connectionId = dest.value("connectionId", 0LL);
        auto connection = db_.getDriveConnection(connectionId);
        if (!connection || connection->userId != userId) throw BadRequest("Connect and choose a Google Drive account");
        int keep = dest.value("keep", 7);
        if (keep < 1 || keep > 365) throw BadRequest("The number of backups to keep must be between 1 and 365");
        out["destination"] = {{"type", "gdrive"}, {"connectionId", connection->id}, {"email", connection->email},
                              {"keep", keep}};
    } else {
        throw BadRequest("Choose where to save the backup");
    }

    json excluded = json::array();
    if (in.contains("excludeFolders")) {
        const json& list = in["excludeFolders"];
        if (!list.is_array() || list.size() > 5000) throw BadRequest("Invalid folder list");
        for (const auto& f : list) excluded.push_back(f.get<std::string>());
    }
    out["excludeFolders"] = excluded;
    return out;
}

json BackupService::validateSchedule(const json& in, const json& config) {
    if (!in.is_object()) throw BadRequest("Schedule is missing");
    std::string frequency = in.value("frequency", "none");
    if (frequency != "none" && frequency != "daily" && frequency != "weekly" && frequency != "monthly") {
        throw BadRequest("Invalid schedule");
    }
    if (frequency != "none" && config["destination"].value("type", "") == "zip") {
        throw BadRequest("A zip file for download cannot be scheduled. Choose Google Drive or an IMAP account.");
    }
    std::string time = in.value("time", "03:00");
    int hour = -1;
    int minute = -1;
    if (time.size() == 5 && time[2] == ':' && std::isdigit(static_cast<unsigned char>(time[0])) &&
        std::isdigit(static_cast<unsigned char>(time[1])) && std::isdigit(static_cast<unsigned char>(time[3])) &&
        std::isdigit(static_cast<unsigned char>(time[4]))) {
        hour = std::stoi(time.substr(0, 2));
        minute = std::stoi(time.substr(3, 2));
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) throw BadRequest("Enter the time as HH:MM");
    int weekday = in.value("weekday", 1);
    if (weekday < 1 || weekday > 7) throw BadRequest("Invalid weekday");
    int monthday = in.value("monthday", 1);
    if (monthday < 1 || monthday > 28) throw BadRequest("Day of the month must be between 1 and 28");
    std::string timezone = in.value("timezone", "UTC");
    try {
        std::chrono::locate_zone(timezone);
    } catch (const std::exception&) {
        throw BadRequest("Unknown time zone: " + timezone);
    }
    return {{"frequency", frequency}, {"time", time},         {"weekday", weekday},
            {"monthday", monthday},   {"timezone", timezone}};
}

std::int64_t BackupService::nextRunAfter(const json& schedule, std::int64_t after) {
    using namespace std::chrono;
    std::string frequency = schedule.value("frequency", "none");
    if (frequency != "daily" && frequency != "weekly" && frequency != "monthly") return 0;

    const time_zone* zone = nullptr;
    try {
        zone = locate_zone(schedule.value("timezone", "UTC"));
    } catch (const std::exception&) {
        zone = locate_zone("UTC");
    }
    std::string time = schedule.value("time", "03:00");
    int hour = 3;
    int minute = 0;
    try {
        hour = std::stoi(time.substr(0, 2));
        minute = std::stoi(time.substr(3, 2));
    } catch (const std::exception&) {
    }
    unsigned wantedWeekday = static_cast<unsigned>(schedule.value("weekday", 1));
    unsigned wantedDay = static_cast<unsigned>(schedule.value("monthday", 1));

    sys_seconds afterTime{seconds{after}};
    local_days today = floor<days>(zone->to_local(afterTime));
    for (int i = 0; i <= 62; ++i) {
        local_days day = today + days{i};
        if (frequency == "weekly" && weekday{day}.iso_encoding() != wantedWeekday) continue;
        if (frequency == "monthly" && static_cast<unsigned>(year_month_day{day}.day()) != wantedDay) continue;
        // Vid omställning till sommartid finns inte alla klockslag; då används övergångstiden.
        sys_seconds when = zone->to_sys(day + hours{hour} + minutes{minute}, choose::earliest);
        if (when > afterTime) return when.time_since_epoch().count();
    }
    return 0;
}

}  // namespace brevduva
