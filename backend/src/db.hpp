#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace brevduva {

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
    std::string signature;
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
    bool useAccountSmtp = true;  // false = egna SMTP-inställningar nedan.
    std::string smtpHost;
    int smtpPort = 587;
    std::string smtpSecurity = "starttls";
    std::string smtpUsername;
    std::string smtpPassword;  // Klartext i minnet, krypterad i databasen.
    bool tlsVerify = true;
};

class Database {
public:
    Database(const std::string& path, std::string encryptionKey);
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
    // Skapar den första administratören om inga användare finns. Konton utan ägare tillfaller den.
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

private:
    std::string encryptOptional(const std::string& plaintext);
    std::string decryptOptional(const std::string& token);
    Identity rowToIdentity(sqlite3_stmt* stmt);
    void exec(const char* sql);
    void migrate();
    Account rowToAccount(sqlite3_stmt* stmt);
    User rowToUser(sqlite3_stmt* stmt);

    sqlite3* db_ = nullptr;
    std::string key_;
    std::recursive_mutex mutex_;
};

}  // namespace brevduva
