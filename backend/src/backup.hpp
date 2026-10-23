#pragma once

#include "config.hpp"
#include "db.hpp"
#include "mail_service.hpp"
#include "oauth.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace brevduva {

using json = nlohmann::json;

// Backup av IMAP-konton till en zip-fil, ett annat IMAP-konto eller Google Drive. Körningar köas i databasen
// och utförs av egna trådar med egna IMAP-anslutningar, så att webbgränssnittet inte blockeras.
// En schemaläggartråd köar sparade backuper när det är dags.
//
// Konfiguration (JSON):
//   { "sourceAccountId": 1,
//     "destination": { "type": "zip" }
//                  | { "type": "imap", "accountId": 2, "folder": "Backup/jobb" }
//                  | { "type": "gdrive", "connectionId": 3, "keep": 7 },
//     "excludeFolders": ["[Gmail]/All Mail"] }
// Schema (JSON):
//   { "frequency": "none" | "daily" | "weekly" | "monthly", "time": "03:00",
//     "weekday": 1-7 (måndag = 1), "monthday": 1-28, "timezone": "Europe/Stockholm" }
class BackupService {
public:
    BackupService(const Config& config, Database& db, MailService& mail, GoogleOAuth& oauth);
    ~BackupService();
    BackupService(const BackupService&) = delete;
    BackupService& operator=(const BackupService&) = delete;

    void start();
    void stop();

    // Kontrollerar inställningarna mot användarens konton och Drive-kopplingar. Kastar BadRequest.
    // Resultatet innehåller även adresser för visning, så att historiken går att läsa om ett konto tas bort.
    json validateConfig(const json& config, long long userId);
    // Kontrollerar ett schema. Kastar BadRequest.
    json validateSchedule(const json& schedule, const json& config);
    // Nästa tidpunkt enligt schemat efter after (Unix-sekunder), eller 0 om schemat saknar tidpunkter.
    static std::int64_t nextRunAfter(const json& schedule, std::int64_t after);

    // Köar en körning. jobId 0 = engångsbackup. Kastar BadRequest om backupen redan körs.
    BackupRun enqueue(long long userId, long long jobId, const json& config);
    // Avbryter en köad eller pågående körning.
    void cancel(long long runId);
    // Tar bort körningens zip-fil från disken.
    void removeFile(const BackupRun& run);

    // Namn på sparade backuper som använder kontot eller Drive-kopplingen.
    std::vector<std::string> jobsUsingAccount(long long userId, long long accountId);
    std::vector<std::string> jobsUsingDrive(long long userId, long long connectionId);

    static constexpr const char* kDriveFolderName = "Brevduva-backup";

private:
    friend class BackupRunner;

    void worker();
    void scheduler();
    void runScheduledJobs(std::int64_t now);
    void cleanup(std::int64_t now);
    void execute(BackupRun run);

    const Config& config_;
    Database& db_;
    MailService& mail_;
    GoogleOAuth& oauth_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool started_ = false;
    bool stopping_ = false;
    std::uint64_t generation_ = 0;  // Räknas upp när en körning köas.
    std::map<long long, std::shared_ptr<std::atomic<bool>>> running_;  // runId -> avbrottsflagga
    std::set<long long> cancelRequested_;  // Avbrutna precis innan de startade.
    std::set<std::string> activeFiles_;    // Filer som pågående körningar skriver till.
    std::vector<std::thread> threads_;
};

}  // namespace brevduva
