#pragma once

#include "config.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct st_mysql;

namespace brevduva {

class Stmt;

struct User {
    long long id = 0;
    std::string username;
    bool isAdmin = false;
    std::int64_t createdAt = 0;
};

struct Account {
    long long id = 0;
    long long userId = 0;
    std::string name;       // Visningsnamn för avsändaren.
    std::string email;
    std::string imapHost;
    int imapPort = 993;
    std::string imapSecurity = "tls";
    std::string smtpHost;
    int smtpPort = 465;
    std::string smtpSecurity = "tls";
    std::string username;
    std::string smtpUsername;  // Tomt = samma som username.
    std::string password;      // Klartext i minnet, krypterad i databasen.
    bool tlsVerify = true;
    std::string signature;      // Ren text. Används i meddelanden utan formatering.
    std::string signatureHtml;  // Valfri formaterad version för meddelanden med formatering.
    std::string authType = "password";  // "password" eller "oauth-google".
    std::string oauthRefreshToken;      // Krypterad i databasen.
    std::string oauthAccessToken;       // Endast i minnet, sätts inför inloggning.

    bool usesGoogleOAuth() const { return authType == "oauth-google"; }
};

// En extra avsändaridentitet för ett e-postkonto, eventuellt med egen SMTP-server.
struct Identity {
    long long id = 0;
    long long accountId = 0;
    std::string name;
    std::string email;
    std::string replyTo;
    std::string signature;
    std::string signatureHtml;
    bool useAccountSmtp = true;  // false = egna SMTP-inställningar nedan.
    std::string smtpHost;
    int smtpPort = 587;
    std::string smtpSecurity = "starttls";
    std::string smtpUsername;
    std::string smtpPassword;  // Klartext i minnet, krypterad i databasen.
    bool tlsVerify = true;
};

// En Google Drive-koppling för backup (egen Google-inloggning med behörigheten drive.file).
struct DriveConnection {
    long long id = 0;
    long long userId = 0;
    std::string email;
    std::string refreshToken;  // Krypterad i databasen.
    std::string folderId;      // Backupmappen i Drive, skapas vid första uppladdningen.
    std::int64_t createdAt = 0;
};

// En sparad backup med schema. config och schedule är JSON och tolkas av BackupService.
struct BackupJob {
    long long id = 0;
    long long userId = 0;
    std::string name;
    std::string config;
    std::string schedule;
    bool enabled = true;
    std::int64_t nextRunAt = 0;  // 0 = inget schemalagt.
    std::int64_t createdAt = 0;
};

// En körning av en backup, manuell eller schemalagd.
struct BackupRun {
    long long id = 0;
    long long userId = 0;
    long long jobId = 0;         // 0 = engångsbackup utan sparat schema.
    std::string status;          // queued, running, done, failed, cancelled
    std::string config;          // JSON, kopia av inställningarna när körningen skapades.
    std::string progress;        // JSON
    std::string message;
    std::string filePath;        // Zip-fil på servern, tom när den tagits bort.
    long long fileSize = 0;
    std::int64_t createdAt = 0;
    std::int64_t startedAt = 0;
    std::int64_t finishedAt = 0;
    std::int64_t expiresAt = 0;  // När zip-filen tas bort, 0 = ingen fil.
};

// En användares inställningar för AI-sortering.
struct AiSettings {
    long long userId = 0;
    std::string baseUrl;       // OpenAI-kompatibelt API, t.ex. Geminis.
    std::string model;
    std::string apiKey;        // Klartext i minnet, krypterad i databasen.
    std::string instructions;  // Användarens egna sorteringsregler i fritext.
};

// Automatisk AI-sortering av nya meddelanden i ett kontos inkorg.
struct AiAutoRule {
    long long accountId = 0;
    long long userId = 0;
    bool enabled = false;
    std::string targets;            // JSON-lista med mappar som AI:n får flytta till.
    int intervalMinutes = 5;
    // Meddelanden med högre UID än lastUid är nya. uidValidity 0 = inte startad än; då sätts märket vid nästa
    // kontroll, så att bara post som kommer efter att regeln slogs på sorteras.
    std::int64_t uidValidity = 0;
    std::int64_t lastUid = 0;
    std::int64_t nextCheckAt = 0;
    std::int64_t lastRunAt = 0;
    std::string lastError;
};

// Det användaren har valt vid manuell AI-sortering av ett konto.
struct AiSortPrefs {
    long long accountId = 0;
    long long userId = 0;
    std::string excluded;  // JSON-lista med mappar som AI:n inte får föreslå. Tom sträng = inget val sparat.
    std::string cursors;   // JSON-objekt: mapp -> {beforeUid, date}, hur långt bakåt mappen har sorterats.
};

// Ett meddelande som den automatiska sorteringen har flyttat.
struct AiSortLogEntry {
    long long id = 0;
    long long userId = 0;
    long long accountId = 0;
    std::string messageId;
    std::string subject;
    std::string sender;
    std::string sourceFolder;
    std::string targetFolder;
    std::string reason;
    std::int64_t movedAt = 0;
    std::int64_t undoneAt = 0;  // 0 = inte ångrad.
    long long jobId = 0;        // Bakgrundsjobbet som gjorde flytten. 0 = den automatiska sorteringen av ny post.
};

// Ett beslut som användaren har tagit om ett meddelande vid AI-sortering. När suggested och chosen skiljer sig åt är
// det en rättning, som AI:n får se som exempel nästa gång. Tom mapp = meddelandet ligger kvar.
struct AiSortExample {
    long long id = 0;
    long long userId = 0;
    long long accountId = 0;
    std::string senderEmail;  // Gemener.
    std::string senderName;
    std::string subject;
    std::string sourceFolder;
    std::string suggested;
    std::string chosen;
    std::int64_t createdAt = 0;
};

// En fast regel: mejl från en avsändare hamnar alltid i samma mapp, utan att AI:n tillfrågas.
struct AiSenderRule {
    long long id = 0;
    long long userId = 0;
    long long accountId = 0;
    std::string sender;      // En adress ("a@exempel.se") eller en domän ("@exempel.se"), gemener.
    std::string target;      // Tom = ligger alltid kvar.
    bool dismissed = false;  // Användaren har tackat nej till en regel för avsändaren; föreslå den inte igen.
    std::int64_t createdAt = 0;
};

// Sortering av en hel mapp i bakgrunden. Jobbet går bakåt från de senaste meddelandena, en omgång i taget.
struct AiSortJob {
    long long id = 0;
    long long userId = 0;
    long long accountId = 0;
    std::string folder;
    std::string targets;  // JSON-lista med mappar som AI:n får flytta till.
    bool unreadOnly = false;
    // running, paused, done, cancelled, undoing (flyttarna görs ogjorda) eller undone.
    std::string state = "running";
    std::int64_t beforeUid = 0;  // Nästa omgång tar meddelanden med lägre UID. 0 = börja med de senaste.
    std::int64_t total = 0;      // Meddelanden att gå igenom, räknat när jobbet startade.
    std::int64_t processed = 0;
    std::int64_t moved = 0;
    std::int64_t movedDelta = 0;  // Ändring sedan förloppet sparades senast. Sparas inte i databasen.
    std::int64_t undoCursor = 0;  // Vid ångra: historikposter med högre id är redan behandlade.
    int failures = 0;             // Fel i rad. Jobbet pausas när de blir för många.
    std::string lastError;
    std::int64_t nextRunAt = 0;
    std::int64_t createdAt = 0;
    std::int64_t finishedAt = 0;
};

// --- Mejlcache ---------------------------------------------------------------------

// En mapp i cachen. total, unread, uidValidity och uidNext är mejlserverns senaste STATUS-värden; synced* är värdena
// när mappens innehåll senast stämdes av, så att en ny avstämning bara görs när något har ändrats.
struct CacheFolder {
    long long id = 0;
    long long accountId = 0;
    std::string path;
    std::string delimiter;
    std::string specialUse;
    bool selectable = true;
    int position = 0;
    std::int64_t total = 0;
    std::int64_t unread = 0;
    std::int64_t uidValidity = 0;
    std::int64_t uidNext = 0;
    std::int64_t syncedUidValidity = 0;
    std::int64_t syncedUidNext = 0;
    std::int64_t syncedTotal = -1;
    std::int64_t syncedUnread = -1;
    bool headersComplete = false;  // Alla meddelandens rubriker finns i cachen, så listor kan visas därifrån.
    std::int64_t dirtyAt = 0;      // Millisekunder. Mappen har ändrats via appen (t.ex. flytt hit) och väntar på avstämning.
    std::int64_t reconciledAt = 0;
    // Ändringsnumret när mappen senast tömdes (omnumrerad av mejlservern). En klient med äldre märke måste
    // hämta om mappen i stället för att följa ändringarna.
    std::int64_t resetModseq = 0;
};

struct CacheFlags {
    bool seen = false, flagged = false, answered = false, draft = false;
    bool operator==(const CacheFlags& o) const {
        return seen == o.seen && flagged == o.flagged && answered == o.answered && draft == o.draft;
    }
};

// Ett meddelandes rubriker i cachen. sender och recipients är JSON-listor med {name, email}.
struct CacheMessage {
    long long id = 0;
    std::uint32_t uid = 0;
    std::string subject;
    std::string sender;
    std::string recipients;
    std::int64_t date = 0;
    std::int64_t size = 0;
    CacheFlags flags;
    bool hasAttachments = false;
    int bodyState = 0;  // Se kBody* nedan.
    std::int64_t modseq = 0;  // Ändringsnummer: sätts varje gång raden skrivs, se Database::cacheSeq.
};

constexpr int kBodyMissing = 0;   // Inte hämtad än.
constexpr int kBodyCached = 1;    // Text, HTML och bilagelista finns.
constexpr int kBodyPartial = 2;   // Stort meddelande: texten finns, men bilagelistan kan vara ofullständig.
constexpr int kBodySkipped = 3;   // Gick inte att cacha (för stor text eller otolkbart); hämtas direkt vid läsning.

struct CacheBody {
    std::string meta;  // JSON: cc, replyTo, messageId, references, attachments.
    std::string text;
    std::string html;
};

struct CacheListQuery {
    int page = 0;
    int pageSize = 50;
    std::uint32_t beforeUid = 0;  // Bara meddelanden med lägre UID. 0 = alla.
    std::string text;
    bool unreadOnly = false;
    bool flaggedOnly = false;
    std::string sort = "uid";  // "uid", "date", "subject", "from", "to" eller "size", se MessageQuery.
    bool descending = true;
};

// Sökning över flera mappar och konton i mejlcachen.
struct CacheSearchQuery {
    std::vector<long long> accountIds;  // Konton att söka i; anroparen har kontrollerat att de är användarens.
    std::string folder;                 // Tom = alla mappar.
    bool includeTrash = false;          // Papperskorg och skräppost tas annars inte med vid sökning i alla mappar.
    bool skipSent = false;              // Utan Skickat och Utkast, för vyn över olästa.
    // Varje grupp ska ge träff, på något av gruppens alternativ: [["kvitto", "faktura"], ["cykel"]].
    std::vector<std::vector<std::string>> terms;
    std::string from;  // Del av avsändarens namn eller adress.
    std::string to;
    std::int64_t after = 0;   // Från och med, unixtid. 0 = ingen gräns.
    std::int64_t before = 0;  // Före, unixtid. 0 = ingen gräns.
    bool unreadOnly = false;
    bool flaggedOnly = false;
    bool attachmentsOnly = false;
    int page = 0;
    int pageSize = 50;
    std::string sort = "date";  // Som i CacheListQuery.
    bool descending = true;
};

struct CacheSearchHit {
    CacheMessage message;
    long long accountId = 0;
    std::string folder;
};

struct CacheSearchResult {
    std::int64_t total = 0;
    std::vector<CacheSearchHit> hits;
};

struct CacheListResult {
    std::int64_t total = 0;
    std::vector<CacheMessage> messages;
};

struct CacheRemoved {
    std::int64_t count = 0;
    std::int64_t unread = 0;
};

// Ändringsräknaren för mejlcachen. Varje skrivning (ny rad, ändrade flaggor, borttagning) får ett nytt, stigande
// nummer, så att en klient som sparat det senaste nummer den sett kan hämta bara det som ändrats sedan dess.
// Numret delas ut inuti den transaktion som skriver raderna, så numrens ordning är också ordningen de blir synliga i.
struct CacheSeq {
    std::int64_t value = 0;   // Senast utdelade nummer.
    std::int64_t pruned = 0;  // Gravstenar till och med detta nummer är rensade; äldre märken måste synka om.
};

struct CacheTombstone {
    std::string folder;
    std::uint32_t uid = 0;
    std::int64_t modseq = 0;
};

struct CacheChange {
    std::string folder;
    CacheMessage message;
};

struct CacheChanges {
    std::vector<CacheChange> messages;
    std::vector<CacheTombstone> removed;
    std::int64_t cutoff = 0;  // Ändringarna täcker numren (since, cutoff].
    bool more = false;        // Det finns fler ändringar efter cutoff.
};

// En enhet (telefon med Android-appen) som får push-notiser. notify är JSON: {"<kontoId>": ["INBOX", ...]};
// tom sträng betyder inkorgen i alla konton.
struct Device {
    long long id = 0;
    long long userId = 0;
    std::string token;  // FCM-registreringstoken.
    std::string name;
    std::string platform;
    std::string appVersion;
    std::string notify;
    std::int64_t createdAt = 0;
    std::int64_t lastSeenAt = 0;
};

struct CacheProgress {
    std::int64_t folders = 0;
    std::int64_t foldersComplete = 0;
    std::int64_t messages = 0;
    std::int64_t bodies = 0;  // Meddelanden vars text är hämtad (eller medvetet hoppas över).
};

class Database {
public:
    // Ansluter till MariaDB och skapar eller uppdaterar schemat. Väntar en stund om servern inte svarar än.
    // migrate = false för extra anslutningar (t.ex. synktrådarnas), som inte ska röra schemat.
    explicit Database(const Config& config, bool migrate = true);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // --- Användare ---
    int countUsers();
    int countAdmins();
    std::vector<User> listUsers();
    std::optional<User> getUser(long long id);
    // Returnerar användaren och dess lösenordshash.
    std::optional<std::pair<User, std::string>> findUserForLogin(const std::string& username);
    long long createUser(const std::string& username, const std::string& passwordHash, bool isAdmin);
    // Skapar den första administratören om inga användare finns.
    std::optional<long long> createFirstAdmin(const std::string& username, const std::string& passwordHash);
    void setUserPassword(long long id, const std::string& passwordHash);
    void setUserAdmin(long long id, bool isAdmin);
    void deleteUser(long long id);  // Tar även bort användarens konton och sessioner.

    // --- Sessioner (token lagras som SHA-256) ---
    void createSession(const std::string& tokenHash, long long userId, std::int64_t expiresAt);
    std::optional<User> userForSession(const std::string& tokenHash, std::int64_t now);
    void deleteSession(const std::string& tokenHash);
    void deleteSessionsForUser(long long userId, const std::string& exceptTokenHash = {});
    void purgeExpiredSessions(std::int64_t now);
    // När sessionen går ut, eller 0 om den inte finns.
    std::int64_t sessionExpiresAt(const std::string& tokenHash);

    // --- E-postkonton ---
    std::vector<Account> listAccounts(long long userId);
    std::optional<Account> getAccount(long long id);
    long long createAccount(const Account& account);
    void updateAccount(const Account& account);
    void deleteAccount(long long id);

    // --- Avsändaridentiteter ---
    std::vector<Identity> listIdentities(long long accountId);
    std::optional<Identity> getIdentity(long long id);
    long long createIdentity(const Identity& identity);
    void updateIdentity(const Identity& identity);
    void deleteIdentity(long long id);

    // --- Google Drive-kopplingar ---
    std::vector<DriveConnection> listDriveConnections(long long userId);
    std::optional<DriveConnection> getDriveConnection(long long id);
    long long createDriveConnection(const DriveConnection& connection);
    void setDriveFolderId(long long id, const std::string& folderId);
    void setDriveRefreshToken(long long id, const std::string& refreshToken);
    void deleteDriveConnection(long long id);

    // --- Backup ---
    std::vector<BackupJob> listBackupJobs(long long userId);
    std::optional<BackupJob> getBackupJob(long long id);
    long long createBackupJob(const BackupJob& job);
    void updateBackupJob(const BackupJob& job);
    void deleteBackupJob(long long id);
    std::vector<BackupJob> dueBackupJobs(std::int64_t now);
    void setBackupJobNextRun(long long id, std::int64_t nextRunAt);

    long long createBackupRun(const BackupRun& run);
    std::optional<BackupRun> getBackupRun(long long id);
    std::vector<BackupRun> listBackupRuns(long long userId, int limit);
    std::optional<BackupRun> lastBackupRun(long long jobId);
    bool hasActiveBackupRun(long long jobId);
    // Markerar den äldsta köade körningen som startad och returnerar den.
    std::optional<BackupRun> claimQueuedBackupRun(std::int64_t now);
    void setBackupRunProgress(long long id, const std::string& progress);
    void finishBackupRun(const BackupRun& run);
    // Avbryter en köad körning. Returnerar false om den redan har startat eller är klar.
    bool cancelQueuedBackupRun(long long id, std::int64_t now);
    void deleteBackupRun(long long id);
    // Körningar som var igång när servern stängdes av markeras som misslyckade.
    void failInterruptedBackupRuns(std::int64_t now);
    std::vector<BackupRun> expiredBackupFiles(std::int64_t now);
    void clearBackupRunFile(long long id);
    std::vector<std::string> backupRunFiles();
    void pruneBackupRuns(std::int64_t olderThan);

    // --- Enheter för push-notiser (Android-appen) ---
    std::vector<Device> listDevices(long long userId);
    // Sparar eller uppdaterar enheten på dess token. En token som hörde till en annan användare flyttas över.
    // Tomma fält (name, platform, appVersion, notify) lämnar det sparade värdet orört.
    void upsertDevice(const Device& device);
    bool deleteDevice(long long userId, const std::string& token);
    void deleteDeviceByToken(const std::string& token);
    // --- Avsändare vars fjärrinnehåll (bilder med mera) alltid visas ---
    std::vector<std::string> listRemoteSenders(long long userId);
    void addRemoteSender(long long userId, const std::string& sender);
    void deleteRemoteSender(long long userId, const std::string& sender);

    // --- AI-sortering ---
    std::optional<AiSettings> getAiSettings(long long userId);
    // Sparar inställningarna som de aktiva, och nyckeln och modellen för just den tjänsten, så att de finns kvar när
    // användaren byter till en annan tjänst och sedan tillbaka.
    void saveAiSettings(const AiSettings& settings);
    // Tar bort inställningarna och alla sparade nycklar.
    void deleteAiSettings(long long userId);
    // Det som är sparat per tjänst (baseUrl, model och apiKey är ifyllda). Den aktiva tjänsten ingår.
    std::vector<AiSettings> listAiProviderKeys(long long userId);

    std::vector<AiSortPrefs> listAiSortPrefs(long long userId);
    std::optional<AiSortPrefs> getAiSortPrefs(long long accountId);
    void saveAiSortCursors(long long userId, long long accountId, const std::string& cursors);
    void saveAiSortExcluded(long long userId, long long accountId, const std::string& excluded);

    std::optional<AiAutoRule> getAiAutoRule(long long accountId);
    std::vector<AiAutoRule> listAiAutoRules(long long userId);
    // Sparar det användaren ställer in. resetState nollställer märket, så att regeln börjar om från nu.
    void saveAiAutoRule(const AiAutoRule& rule, bool resetState);
    // Sparar var sorteringen befinner sig efter en kontroll.
    void setAiAutoRuleState(const AiAutoRule& rule);
    std::vector<AiAutoRule> dueAiAutoRules(std::int64_t now);

    // --- Det AI-sorteringen har lärt sig ---
    // Sparar beslutet om det inte redan finns ett likadant. Returnerar false om det fanns.
    bool addAiSortExample(const AiSortExample& example);
    void pruneAiSortExamples(long long accountId, int keep);
    std::vector<AiSortExample> listAiSortCorrections(long long userId, int limit);
    // Rättningar för kontot, nyast först. sender är en adress, en domän ("@exempel.se") eller tom för alla.
    std::vector<AiSortExample> aiSortCorrectionsFor(long long accountId, const std::string& sender, int limit);
    // De senaste besluten för en avsändare, nyast först.
    std::vector<AiSortExample> aiSortDecisionsFor(long long accountId, const std::string& senderEmail, int limit);
    // Avsändare med minst minCount beslut, de senast aktiva först: (konto, adress).
    std::vector<std::pair<long long, std::string>> aiSortFrequentSenders(long long userId, int minCount, int limit);
    bool deleteAiSortExample(long long id, long long userId);
    void clearAiSortExamples(long long userId);

    void saveAiSenderRule(const AiSenderRule& rule);
    std::vector<AiSenderRule> listAiSenderRules(long long userId);
    std::vector<AiSenderRule> aiSenderRulesFor(long long accountId);
    bool deleteAiSenderRule(long long id, long long userId);
    // En mapp har bytt namn: regler och exempel ska peka på den nya sökvägen.
    void renameAiLearningFolder(long long accountId, const std::string& from, const std::string& to);

    long long addAiSortJob(const AiSortJob& job);
    std::optional<AiSortJob> getAiSortJob(long long id);
    std::vector<AiSortJob> listAiSortJobs(long long userId, int limit);
    // Jobb som arbetar (running eller undoing) och vars väntetid har gått ut.
    std::vector<AiSortJob> dueAiSortJobs(std::int64_t now);
    bool hasActiveAiSortJob(long long accountId);
    // Sparar förloppet. Läget rörs inte, så en paus som användaren gör under en omgång skrivs inte över.
    // Antalet flyttade sparas som en ändring (movedDelta), så att en enskild ångring under tiden inte skrivs över.
    void setAiSortJobProgress(AiSortJob& job);
    void addAiSortJobMoved(long long id, std::int64_t delta);
    // Byter läge om jobbet är i något av lägena i from. Returnerar false om det inte var det.
    bool setAiSortJobState(long long id, const std::vector<std::string>& from, const std::string& to, std::int64_t now);
    void renameAiSortJobFolder(long long accountId, const std::string& from, const std::string& to);
    // Flyttade meddelanden per målmapp, utan de ångrade.
    std::map<std::string, std::int64_t> aiSortJobTargets(long long jobId);
    // Jobbets flyttar som inte är ångrade, nyast först, med id lägre än beforeId (0 = från början).
    std::vector<AiSortLogEntry> aiSortJobMoves(long long jobId, long long beforeId, int limit);

    long long addAiSortLog(const AiSortLogEntry& entry);
    std::optional<AiSortLogEntry> getAiSortLog(long long id);
    std::vector<AiSortLogEntry> listAiSortLog(long long userId, int limit);
    void markAiSortLogUndone(long long id, std::int64_t now);
    void deleteAiSortLog(long long id);
    // Sant om meddelandet redan har sorterats automatiskt (även om flytten ångrats), så att det inte sorteras igen.
    bool wasAiSorted(long long accountId, const std::string& messageId);
    void pruneAiSortLog(std::int64_t olderThan);

    // --- Mejlcache (db_cache.cpp) ---
    // Transaktioner får bara användas på en anslutning som en enda tråd äger, t.ex. en synktråds.
    void begin();
    void commit();
    void rollback();

    std::vector<CacheFolder> listCacheFolders(long long accountId);
    std::optional<CacheFolder> getCacheFolder(long long accountId, const std::string& path);
    // Sparar mejlserverns mapplista: nya mappar läggs till, försvunna tas bort med sina meddelanden.
    void saveCacheFolderList(long long accountId, const std::vector<CacheFolder>& folders);
    // Sparar avstämningens resultat. Mappen slutar vara smutsig om den inte ändrats sedan avstämningen började.
    void saveCacheFolderSync(const CacheFolder& folder, std::int64_t startedAtMs);
    void markCacheFolderDirty(long long accountId, const std::string& path, std::int64_t nowMs);
    // Justerar mappens antal efter en ändring via appen. alsoSynced = true när cachens innehåll ändrats på samma sätt,
    // så att ändringen inte i sig utlöser en ny avstämning mot mejlservern.
    void adjustCacheFolderCounts(long long folderId, std::int64_t total, std::int64_t unread, bool alsoSynced);
    struct CacheCounts {
        std::int64_t count = 0;
        std::int64_t unread = 0;
        std::int64_t maxUid = 0;
    };
    CacheCounts cacheCounts(long long folderId);
    // Byter sökväg på mappen och dess undermappar, så att cachade meddelanden följer med vid namnbyte och flytt.
    void renameCacheFolder(long long accountId, const std::string& from, const std::string& to);
    void deleteCacheFolder(long long accountId, const std::string& path);
    void purgeCacheMessages(long long folderId);

    std::map<std::uint32_t, CacheFlags> cacheMessageFlags(long long folderId);
    void upsertCacheMessage(long long folderId, const CacheMessage& message);
    // Flera rader med samma ändringsnummer, så att en synkomgång inte delar ut ett nummer per meddelande.
    void upsertCacheMessages(long long folderId, const std::vector<CacheMessage>& messages);
    void setCacheMessageFlags(long long folderId, std::uint32_t uid, const CacheFlags& flags);
    // Ändrar flaggor på flera meddelanden och returnerar hur antalet olästa ändrades.
    std::int64_t changeCacheFlags(long long folderId, const std::vector<std::uint32_t>& uids, const CacheFlags& add,
                                  const CacheFlags& remove);
    CacheRemoved removeCacheMessages(long long folderId, const std::vector<std::uint32_t>& uids);
    // Meddelanden utan hämtad text, nyast först.
    std::vector<CacheMessage> cacheMessagesWithoutBody(long long folderId, int limit);
    void saveCacheBody(long long messageRowId, int state, const CacheBody& body, const std::string& searchText);
    void setCacheBodyState(long long messageRowId, int state);

    CacheListResult listCacheMessages(long long folderId, const CacheListQuery& query);
    CacheSearchResult searchCacheMessages(const CacheSearchQuery& query);
    std::optional<CacheMessage> getCacheMessage(long long folderId, std::uint32_t uid);
    std::optional<CacheBody> getCacheBody(long long messageRowId);
    CacheProgress cacheProgress(long long accountId);
    // --- Ändringar för klienter med egen kopia av cachen (appen) ---
    CacheSeq cacheSeq();
    // Ändringar i kontots mappar med nummer i (since, cutoff], äldst först. Högst limit meddelanderader, men ett
    // nummers rader delas aldrig upp; blir det fler sätts more och cutoff till det sista hela numret.
    CacheChanges listCacheChanges(long long accountId, std::int64_t since, std::int64_t cutoff, int limit);
    // Tar bort gravstenar äldre än olderThan och flyttar fram CacheSeq::pruned.
    void pruneCacheTombstones(std::int64_t olderThan);
    // Alla konton, för synktrådarna.
    std::vector<Account> listAllAccounts();

private:
    std::string encryptOptional(const std::string& plaintext);
    std::string decryptOptional(const std::string& token);
    void exec(const std::string& sql);
    void migrate();
    Identity rowToIdentity(Stmt& s);
    AiAutoRule rowToAiAutoRule(Stmt& s);
    AiSortLogEntry rowToAiSortLogEntry(Stmt& s);
    AiSortJob rowToAiSortJob(Stmt& s);
    DriveConnection rowToDriveConnection(Stmt& s);
    BackupJob rowToBackupJob(Stmt& s);
    BackupRun rowToBackupRun(Stmt& s);
    Account rowToAccount(Stmt& s);
    User rowToUser(Stmt& s);

    // Nästa ändringsnummer. Ska anropas inuti den transaktion som skriver raderna (se CacheSeq).
    std::int64_t allocModseq();
    // Kör f i en transaktion. Pågår redan en (begin() på en synktråd) används den.
    void writeTx(const std::function<void()>& f);

    st_mysql* db_ = nullptr;
    std::string key_;
    std::recursive_mutex mutex_;
    bool inTx_ = false;
};

}  // namespace brevduva
