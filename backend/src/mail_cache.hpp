#pragma once

#include "config.hpp"
#include "db.hpp"
#include "imap.hpp"
#include "mime.hpp"
#include "push.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace brevduva {

using json = nlohmann::json;

class MailService;
class RawCache;
struct MessageQuery;
struct MessageRef;

// Mejlcache i databasen. Synktrådar med egna IMAP- och databasanslutningar speglar mapplistan, alla meddelandens
// rubriker och flaggor samt meddelandetexten (inte bilagor). Webbgränssnittet läser därifrån, vilket går fort och
// fungerar även när mejlservern inte går att nå. Ändringar (flytta, ta bort, flagga) görs som förut direkt mot
// mejlservern av MailService, som sedan uppdaterar cachen via metoderna under "Ändringar".
//
// Gmails "Alla mail" och "Stjärnmärkt" cachas inte; de innehåller bara kopior och läses direkt från mejlservern.
class MailCache {
public:
    // push = nullptr när push-notiser inte är konfigurerade.
    MailCache(const Config& config, Database& db, MailService& mail, RawCache& rawCache, PushService* push = nullptr);
    ~MailCache();
    MailCache(const MailCache&) = delete;
    MailCache& operator=(const MailCache&) = delete;

    void start();
    void stop();

    // --- Läsning. nullopt betyder att svaret inte finns i cachen och ska hämtas från mejlservern. ---
    // force = true används när mejlservern inte svarar: då duger även en mapplista eller lista som kan vara inaktuell.
    std::optional<json> folders(long long accountId, bool force);
    std::optional<json> messages(long long accountId, const MessageQuery& query, bool force);
    std::optional<json> message(long long accountId, const MessageRef& ref, bool force);
    // Söker i det som finns i cachen, över flera mappar och konton. Varje träff har accountId och folder.
    json search(const CacheSearchQuery& query);
    // Sant när synken inte når kontots mejlserver. Då görs inga försök att läsa därifrån, som bara skulle ge väntetid.
    bool offline(long long accountId);
    // Webbgränssnittets anslutning nådde inte mejlservern. Kontot räknas som onåbart direkt, utan att vänta på att
    // synken ska märka det, och synken får pröva på en gång.
    void reportUnreachable(long long accountId, const std::string& error);

    // Svaret till klienten för ett tolkat meddelande, utan flaggor. Används också när texten sparas i cachen.
    static json bodyJson(const mime::ParsedMessage& msg);

    // --- Ändringar som MailService har gjort mot mejlservern ---
    void storeBody(long long accountId, const MessageRef& ref, const mime::ParsedMessage& msg);
    void flagsChanged(long long accountId, const std::string& folder, const std::vector<std::uint32_t>& uids,
                      const std::vector<std::string>& add, const std::vector<std::string>& remove);
    // target är mappen meddelandena flyttades till, eller tom om de raderades.
    void messagesRemoved(long long accountId, const std::string& folder, const std::vector<std::uint32_t>& uids,
                         const std::string& target);
    // Mappen har fått nytt innehåll (t.ex. en kopia i Skickat) och ska stämmas av snarast.
    void folderTouched(long long accountId, const std::string& folder);
    void foldersChanged(long long accountId);
    void folderRenamed(long long accountId, const std::string& from, const std::string& to);
    void folderDeleted(long long accountId, const std::string& path);
    // Inloggningsuppgifterna har ändrats; synken ska ansluta på nytt.
    void accountChanged(long long accountId);
    // Märker meddelandet som läst på mejlservern i bakgrunden, så att läsningen inte väntar på det.
    void markSeenLater(long long accountId, const MessageRef& ref);

    // --- Ändringar sedan ett ändringsnummer, för appen som har en egen kopia av cachen ---
    // since = 0 ger bara mapplistan och det aktuella numret. Kastar Gone om since är för gammalt (gravstenarna är
    // rensade) eller ligger före en återställd databas; då ska klienten hämta om kontot.
    json changes(long long accountId, std::int64_t since, int limit);

    // --- Status ---
    json status(const std::vector<Account>& accounts);
    // Startar en synk av kontot nu och väntar högst waitSeconds på att den blir klar.
    json syncNow(const Account& account, int waitSeconds);

private:
    struct AccountState {
        bool syncing = false;
        bool offline = false;  // Senaste synkförsöket misslyckades; läs från cachen utan att fråga mejlservern.
        std::string error;
        std::int64_t lastSyncAt = 0;        // Senaste lyckade synk.
        std::int64_t lastFolderListAt = 0;  // Mapplistan i cachen är så här färsk.
        std::int64_t nextPassAt = 0;
        int failures = 0;
        bool kicked = false;
        bool reconnect = false;
        // Räknas upp när mappar skapas, byter namn eller tas bort via appen, så att en mapplista som hämtades
        // strax innan inte räknas som färsk.
        std::uint64_t foldersVersion = 0;
        std::uint64_t generation = 0;  // Räknas upp efter varje synkförsök.
    };
    struct Budget {
        int headers = 0;
        int bodies = 0;
        std::int64_t bodyBytes = 0;
    };

    void worker(int index);
    void deferredWorker();
    // Returnerar true om det finns mer att hämta, så att nästa pass ska börja direkt.
    bool syncAccount(Database& wdb, ImapClient& client, const Account& account);
    // freshSince: meddelanden som kom till mejlservern före den tiden ger ingen push-notis (de är inte nya för
    // användaren, bara nya i cachen, t.ex. efter en omstart eller en flytt mellan mappar).
    bool reconcileFolder(Database& wdb, ImapClient& client, const Account& account, CacheFolder& folder, Budget& budget,
                         std::int64_t startedAt, std::int64_t freshSince);
    bool fetchHeaders(Database& wdb, ImapClient& client, const Account& account, const CacheFolder& folder,
                      std::vector<std::uint32_t> missing, Budget& budget, std::int64_t freshSince);
    bool fetchBodies(Database& wdb, ImapClient& client, const CacheFolder& folder, Budget& budget);
    void kick(long long accountId);
    json stateJson(const Account& account);
    static bool cacheable(const CacheFolder& folder);

    const Config& config_;
    Database& db_;  // Huvudanslutningen, för läsning och ändringar från webbgränssnittet.
    MailService& mail_;
    RawCache& rawCache_;
    PushService* push_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<long long, AccountState> states_;
    std::deque<std::function<void()>> deferred_;
    bool started_ = false;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace brevduva
