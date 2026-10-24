#include "trash_purger.hpp"

#include "mail_cache.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>

namespace brevduva {

namespace {

constexpr int kFirstPassDelay = 60;  // Sekunder efter start innan första passet, så att uppstarten får gå före.
constexpr std::size_t kChunk = 500;  // Så många meddelanden tas bort per IMAP-kommando.

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

TrashPurger::TrashPurger(const Config& config, Database& db, MailService& mail, MailCache* cache)
    : config_(config), db_(db), mail_(mail), cache_(cache) {}

TrashPurger::~TrashPurger() {
    stop();
}

void TrashPurger::start() {
    std::lock_guard lock(mutex_);
    if (started_) return;
    started_ = true;
    thread_ = std::thread(&TrashPurger::loop, this);
}

void TrashPurger::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopping_) return;
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TrashPurger::kick() {
    {
        std::lock_guard lock(mutex_);
        kicked_ = true;
    }
    wake_.notify_all();
}

void TrashPurger::loop() {
    std::unique_ptr<Database> wdb;
    int waitSeconds = kFirstPassDelay;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(waitSeconds), [&] { return stopping_ || kicked_; });
            if (stopping_) return;
            kicked_ = false;
        }
        try {
            if (!wdb) wdb = std::make_unique<Database>(config_, false);
            pass(*wdb);
        } catch (const std::exception& e) {
            std::cerr << "[trash] " << e.what() << "\n";
            wdb.reset();
        }
        waitSeconds = config_.trashPurgeInterval;
    }
}

void TrashPurger::pass(Database& wdb) {
    for (const auto& user : wdb.listUsers()) {
        if (user.trashDays <= 0) continue;
        for (const auto& account : wdb.listAccounts(user.id)) {
            {
                std::lock_guard lock(mutex_);
                if (stopping_) return;
            }
            try {
                purgeAccount(wdb, account, user.trashDays);
            } catch (const std::exception& e) {
                std::cerr << "[trash] account " << account.id << ": " << e.what() << "\n";
            }
        }
    }
}

void TrashPurger::purgeAccount(Database& wdb, const Account& account, int days) {
    auto client = mail_.connectImap(account);
    std::string trash;
    for (const auto& f : client->listFolders(false)) {
        if (!f.attributes.count("\\noselect") && specialUseOf(f) == "trash") {
            trash = f.path;
            break;
        }
    }
    if (trash.empty()) {
        client->logout();
        return;
    }

    std::uint32_t uidValidity = client->status(trash).uidValidity;
    client->select(trash);
    std::vector<std::uint32_t> uids = client->uidSearch(ImapCommand("ALL"), false);
    std::sort(uids.begin(), uids.end());

    std::int64_t t = now();
    auto seen = wdb.trashSeen(account.id, uidValidity);
    std::vector<std::uint32_t> fresh, gone, expired;
    for (auto uid : uids) {
        auto it = seen.find(uid);
        if (it == seen.end()) {
            fresh.push_back(uid);
        } else if (it->second <= t - static_cast<std::int64_t>(days) * config_.trashDaySeconds) {
            expired.push_back(uid);
        }
    }
    std::set<std::uint32_t> present(uids.begin(), uids.end());
    for (const auto& [uid, firstSeen] : seen) {
        if (!present.count(uid)) gone.push_back(uid);
    }
    // Raderna för andra UIDVALIDITY tas bort här också, så de nya raderna beskriver mappen som den är nu.
    if (!fresh.empty() || seen.empty()) wdb.recordTrashSeen(account.id, uidValidity, fresh, t);
    if (!gone.empty()) wdb.deleteTrashSeen(account.id, uidValidity, gone);

    for (std::size_t i = 0; i < expired.size(); i += kChunk) {
        std::vector<std::uint32_t> chunk(expired.begin() + i,
                                         expired.begin() + std::min(expired.size(), i + kChunk));
        std::string set = ImapClient::uidSetFrom(chunk);
        client->uidStore(set, "+FLAGS.SILENT", "\\Deleted");
        client->expungeUids(set);
        wdb.deleteTrashSeen(account.id, uidValidity, chunk);
        if (cache_) cache_->messagesRemoved(account.id, trash, chunk, "");
    }
    client->logout();
    if (!expired.empty()) {
        std::cout << "[trash] account " << account.id << ": removed " << expired.size() << " message"
                  << (expired.size() == 1 ? "" : "s") << " that had been in " << trash << " for more than " << days
                  << " day" << (days == 1 ? "" : "s") << std::endl;
    }
}

}  // namespace brevduva
