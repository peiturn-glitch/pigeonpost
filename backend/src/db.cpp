#include "db.hpp"

#include "crypto.hpp"
#include "errors.hpp"

#include <memory>
#include <stdexcept>

namespace brevduva {

namespace {

using Lock = std::lock_guard<std::recursive_mutex>;

// Version 1: användare, sessioner och ägare för konton.
// Version 2: avsändaridentiteter (tabellen skapas via kSchema).
// Version 3: OAuth-inloggning för konton (auth_type, oauth_token_enc).
constexpr int kSchemaVersion = 3;

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash TEXT NOT NULL,
    is_admin      INTEGER NOT NULL DEFAULT 0,
    created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);

CREATE TABLE IF NOT EXISTS accounts (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    name          TEXT NOT NULL,
    email         TEXT NOT NULL,
    imap_host     TEXT NOT NULL,
    imap_port     INTEGER NOT NULL,
    imap_security TEXT NOT NULL,
    smtp_host     TEXT NOT NULL,
    smtp_port     INTEGER NOT NULL,
    smtp_security TEXT NOT NULL,
    username      TEXT NOT NULL,
    smtp_username TEXT NOT NULL DEFAULT '',
    password_enc  TEXT NOT NULL,
    tls_verify    INTEGER NOT NULL DEFAULT 1,
    signature     TEXT NOT NULL DEFAULT '',
    created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS identities (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id        INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name              TEXT NOT NULL,
    email             TEXT NOT NULL,
    reply_to          TEXT NOT NULL DEFAULT '',
    signature         TEXT NOT NULL DEFAULT '',
    use_account_smtp  INTEGER NOT NULL DEFAULT 1,
    smtp_host         TEXT NOT NULL DEFAULT '',
    smtp_port         INTEGER NOT NULL DEFAULT 587,
    smtp_security     TEXT NOT NULL DEFAULT 'starttls',
    smtp_username     TEXT NOT NULL DEFAULT '',
    smtp_password_enc TEXT NOT NULL DEFAULT '',
    tls_verify        INTEGER NOT NULL DEFAULT 1,
    created_at        INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);
CREATE INDEX IF NOT EXISTS identities_account ON identities(account_id);
)SQL";

const char* kIdentityColumns =
    "id, account_id, name, email, reply_to, signature, use_account_smtp, smtp_host, smtp_port, smtp_security, "
    "smtp_username, smtp_password_enc, tls_verify";

const char* kAccountColumns =
    "id, user_id, name, email, imap_host, imap_port, imap_security, smtp_host, smtp_port, smtp_security, "
    "username, smtp_username, password_enc, tls_verify, signature, auth_type, oauth_token_enc";

const char* kUserColumns = "id, username, is_admin, created_at";

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQL-fel: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, long long v) {
        sqlite3_bind_int64(stmt_, i, v);
        return *this;
    }
    Stmt& bind(int i, int v) {
        sqlite3_bind_int(stmt_, i, v);
        return *this;
    }
    Stmt& bind(int i, bool v) { return bind(i, v ? 1 : 0); }

    bool row() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQL-fel: ") + sqlite3_errmsg(db_));
        return false;
    }
    // Kör en sats som inte returnerar rader. Returnerar sqlite-koden.
    int run() {
        int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
            throw std::runtime_error(std::string("SQL-fel: ") + sqlite3_errmsg(db_));
        }
        return rc;
    }
    sqlite3_stmt* get() { return stmt_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

std::string text(sqlite3_stmt* stmt, int i) {
    const unsigned char* t = sqlite3_column_text(stmt, i);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

}  // namespace

Database::Database(const std::string& path, std::string encryptionKey) : key_(std::move(encryptionKey)) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "okänt fel";
        throw std::runtime_error("Kunde inte öppna databasen " + path + ": " + err);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec(kSchema);
    migrate();
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "okänt fel";
        sqlite3_free(err);
        throw std::runtime_error("SQL-fel: " + msg);
    }
}

void Database::migrate() {
    Lock lock(mutex_);
    Stmt version(db_, "PRAGMA user_version");
    version.row();
    int current = sqlite3_column_int(version.get(), 0);
    if (current >= kSchemaVersion) return;

    if (current < 1) {
        // Databaser från enanvändarversionen saknar user_id på kontona.
        bool hasUserId = false;
        Stmt columns(db_, "PRAGMA table_info(accounts)");
        while (columns.row()) {
            if (text(columns.get(), 1) == "user_id") hasUserId = true;
        }
        if (!hasUserId) exec("ALTER TABLE accounts ADD COLUMN user_id INTEGER REFERENCES users(id)");
        exec("CREATE INDEX IF NOT EXISTS accounts_user ON accounts(user_id)");
    }
    if (current < 3) {
        exec("ALTER TABLE accounts ADD COLUMN auth_type TEXT NOT NULL DEFAULT 'password'");
        exec("ALTER TABLE accounts ADD COLUMN oauth_token_enc TEXT NOT NULL DEFAULT ''");
    }
    exec(("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
}

// Tomma hemligheter lagras som tom sträng i stället för chiffertext.
std::string Database::encryptOptional(const std::string& plaintext) {
    return plaintext.empty() ? std::string() : crypto::encrypt(key_, plaintext);
}

std::string Database::decryptOptional(const std::string& token) {
    return token.empty() ? std::string() : crypto::decrypt(key_, token);
}

// --- Användare ----------------------------------------------------------------

User Database::rowToUser(sqlite3_stmt* stmt) {
    User u;
    u.id = sqlite3_column_int64(stmt, 0);
    u.username = text(stmt, 1);
    u.isAdmin = sqlite3_column_int(stmt, 2) != 0;
    u.createdAt = sqlite3_column_int64(stmt, 3);
    return u;
}

int Database::countUsers() {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM users");
    s.row();
    return sqlite3_column_int(s.get(), 0);
}

int Database::countAdmins() {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM users WHERE is_admin = 1");
    s.row();
    return sqlite3_column_int(s.get(), 0);
}

std::vector<User> Database::listUsers() {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kUserColumns + " FROM users ORDER BY username COLLATE NOCASE").c_str());
    std::vector<User> out;
    while (s.row()) out.push_back(rowToUser(s.get()));
    return out;
}

std::optional<User> Database::getUser(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kUserColumns + " FROM users WHERE id = ?").c_str());
    s.bind(1, id);
    if (s.row()) return rowToUser(s.get());
    return std::nullopt;
}

std::optional<std::pair<User, std::string>> Database::findUserForLogin(const std::string& username) {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kUserColumns + ", password_hash FROM users WHERE username = ?").c_str());
    s.bind(1, username);
    if (s.row()) return std::make_pair(rowToUser(s.get()), text(s.get(), 4));
    return std::nullopt;
}

long long Database::createUser(const std::string& username, const std::string& passwordHash, bool isAdmin) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO users (username, password_hash, is_admin) VALUES (?, ?, ?)");
    s.bind(1, username).bind(2, passwordHash).bind(3, isAdmin);
    if (s.run() == SQLITE_CONSTRAINT) throw BadRequest("Användarnamnet är redan upptaget");
    return sqlite3_last_insert_rowid(db_);
}

std::optional<long long> Database::createFirstAdmin(const std::string& username, const std::string& passwordHash) {
    Lock lock(mutex_);
    exec("BEGIN IMMEDIATE");
    try {
        if (countUsers() > 0) {
            exec("ROLLBACK");
            return std::nullopt;
        }
        long long id = createUser(username, passwordHash, true);
        Stmt adopt(db_, "UPDATE accounts SET user_id = ? WHERE user_id IS NULL");
        adopt.bind(1, id).run();
        exec("COMMIT");
        return id;
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

void Database::setUserPassword(long long id, const std::string& passwordHash) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE users SET password_hash = ? WHERE id = ?");
    s.bind(1, passwordHash).bind(2, id).run();
}

void Database::setUserAdmin(long long id, bool isAdmin) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE users SET is_admin = ? WHERE id = ?");
    s.bind(1, isAdmin).bind(2, id).run();
}

void Database::deleteUser(long long id) {
    Lock lock(mutex_);
    exec("BEGIN IMMEDIATE");
    try {
        Stmt(db_, "DELETE FROM accounts WHERE user_id = ?").bind(1, id).run();
        Stmt(db_, "DELETE FROM sessions WHERE user_id = ?").bind(1, id).run();
        Stmt(db_, "DELETE FROM users WHERE id = ?").bind(1, id).run();
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

// --- Sessioner ----------------------------------------------------------------

void Database::createSession(const std::string& tokenHash, long long userId, std::int64_t expiresAt) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)");
    s.bind(1, tokenHash).bind(2, userId).bind(3, static_cast<long long>(expiresAt)).run();
}

std::optional<User> Database::userForSession(const std::string& tokenHash, std::int64_t now) {
    Lock lock(mutex_);
    Stmt s(db_,
           "SELECT u.id, u.username, u.is_admin, u.created_at FROM sessions s JOIN users u ON u.id = s.user_id "
           "WHERE s.token_hash = ? AND s.expires_at > ?");
    s.bind(1, tokenHash).bind(2, static_cast<long long>(now));
    if (s.row()) return rowToUser(s.get());
    return std::nullopt;
}

void Database::deleteSession(const std::string& tokenHash) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM sessions WHERE token_hash = ?").bind(1, tokenHash).run();
}

void Database::deleteSessionsForUser(long long userId, const std::string& exceptTokenHash) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM sessions WHERE user_id = ? AND token_hash != ?").bind(1, userId).bind(2, exceptTokenHash).run();
}

void Database::purgeExpiredSessions(std::int64_t now) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM sessions WHERE expires_at <= ?").bind(1, static_cast<long long>(now)).run();
}

// --- E-postkonton -------------------------------------------------------------

Account Database::rowToAccount(sqlite3_stmt* stmt) {
    Account a;
    a.id = sqlite3_column_int64(stmt, 0);
    a.userId = sqlite3_column_int64(stmt, 1);
    a.name = text(stmt, 2);
    a.email = text(stmt, 3);
    a.imapHost = text(stmt, 4);
    a.imapPort = sqlite3_column_int(stmt, 5);
    a.imapSecurity = text(stmt, 6);
    a.smtpHost = text(stmt, 7);
    a.smtpPort = sqlite3_column_int(stmt, 8);
    a.smtpSecurity = text(stmt, 9);
    a.username = text(stmt, 10);
    a.smtpUsername = text(stmt, 11);
    a.password = decryptOptional(text(stmt, 12));
    a.tlsVerify = sqlite3_column_int(stmt, 13) != 0;
    a.signature = text(stmt, 14);
    a.authType = text(stmt, 15);
    a.oauthRefreshToken = decryptOptional(text(stmt, 16));
    return a;
}

std::vector<Account> Database::listAccounts(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kAccountColumns + " FROM accounts WHERE user_id = ? ORDER BY id").c_str());
    s.bind(1, userId);
    std::vector<Account> out;
    while (s.row()) out.push_back(rowToAccount(s.get()));
    return out;
}

std::optional<Account> Database::getAccount(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kAccountColumns + " FROM accounts WHERE id = ?").c_str());
    s.bind(1, id);
    if (s.row()) return rowToAccount(s.get());
    return std::nullopt;
}

long long Database::createAccount(const Account& a) {
    Lock lock(mutex_);
    Stmt s(db_,
           "INSERT INTO accounts (user_id, name, email, imap_host, imap_port, imap_security, smtp_host, smtp_port, "
           "smtp_security, username, smtp_username, password_enc, tls_verify, signature, auth_type, oauth_token_enc) "
           "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, a.userId).bind(2, a.name).bind(3, a.email).bind(4, a.imapHost).bind(5, a.imapPort)
        .bind(6, a.imapSecurity).bind(7, a.smtpHost).bind(8, a.smtpPort).bind(9, a.smtpSecurity)
        .bind(10, a.username).bind(11, a.smtpUsername).bind(12, encryptOptional(a.password))
        .bind(13, a.tlsVerify).bind(14, a.signature).bind(15, a.authType).bind(16, encryptOptional(a.oauthRefreshToken));
    if (s.run() != SQLITE_DONE) throw std::runtime_error(std::string("Kunde inte spara kontot: ") + sqlite3_errmsg(db_));
    return sqlite3_last_insert_rowid(db_);
}

void Database::updateAccount(const Account& a) {
    Lock lock(mutex_);
    Stmt s(db_,
           "UPDATE accounts SET name=?, email=?, imap_host=?, imap_port=?, imap_security=?, smtp_host=?, "
           "smtp_port=?, smtp_security=?, username=?, smtp_username=?, password_enc=?, tls_verify=?, "
           "signature=?, auth_type=?, oauth_token_enc=? WHERE id=?");
    s.bind(1, a.name).bind(2, a.email).bind(3, a.imapHost).bind(4, a.imapPort).bind(5, a.imapSecurity)
        .bind(6, a.smtpHost).bind(7, a.smtpPort).bind(8, a.smtpSecurity).bind(9, a.username)
        .bind(10, a.smtpUsername).bind(11, encryptOptional(a.password)).bind(12, a.tlsVerify)
        .bind(13, a.signature).bind(14, a.authType).bind(15, encryptOptional(a.oauthRefreshToken)).bind(16, a.id);
    if (s.run() != SQLITE_DONE) {
        throw std::runtime_error(std::string("Kunde inte uppdatera kontot: ") + sqlite3_errmsg(db_));
    }
}

void Database::deleteAccount(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM accounts WHERE id = ?").bind(1, id).run();
}

// --- Avsändaridentiteter --------------------------------------------------------

Identity Database::rowToIdentity(sqlite3_stmt* stmt) {
    Identity i;
    i.id = sqlite3_column_int64(stmt, 0);
    i.accountId = sqlite3_column_int64(stmt, 1);
    i.name = text(stmt, 2);
    i.email = text(stmt, 3);
    i.replyTo = text(stmt, 4);
    i.signature = text(stmt, 5);
    i.useAccountSmtp = sqlite3_column_int(stmt, 6) != 0;
    i.smtpHost = text(stmt, 7);
    i.smtpPort = sqlite3_column_int(stmt, 8);
    i.smtpSecurity = text(stmt, 9);
    i.smtpUsername = text(stmt, 10);
    std::string enc = text(stmt, 11);
    i.smtpPassword = decryptOptional(enc);
    i.tlsVerify = sqlite3_column_int(stmt, 12) != 0;
    return i;
}

std::vector<Identity> Database::listIdentities(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kIdentityColumns + " FROM identities WHERE account_id = ? ORDER BY id").c_str());
    s.bind(1, accountId);
    std::vector<Identity> out;
    while (s.row()) out.push_back(rowToIdentity(s.get()));
    return out;
}

std::optional<Identity> Database::getIdentity(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, (std::string("SELECT ") + kIdentityColumns + " FROM identities WHERE id = ?").c_str());
    s.bind(1, id);
    if (s.row()) return rowToIdentity(s.get());
    return std::nullopt;
}

long long Database::createIdentity(const Identity& i) {
    Lock lock(mutex_);
    Stmt s(db_,
           "INSERT INTO identities (account_id, name, email, reply_to, signature, use_account_smtp, smtp_host, "
           "smtp_port, smtp_security, smtp_username, smtp_password_enc, tls_verify) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, i.accountId).bind(2, i.name).bind(3, i.email).bind(4, i.replyTo).bind(5, i.signature)
        .bind(6, i.useAccountSmtp).bind(7, i.smtpHost).bind(8, i.smtpPort).bind(9, i.smtpSecurity)
        .bind(10, i.smtpUsername).bind(11, encryptOptional(i.smtpPassword))
        .bind(12, i.tlsVerify);
    if (s.run() != SQLITE_DONE) throw std::runtime_error(std::string("Kunde inte spara avsändaren: ") + sqlite3_errmsg(db_));
    return sqlite3_last_insert_rowid(db_);
}

void Database::updateIdentity(const Identity& i) {
    Lock lock(mutex_);
    Stmt s(db_,
           "UPDATE identities SET name=?, email=?, reply_to=?, signature=?, use_account_smtp=?, smtp_host=?, "
           "smtp_port=?, smtp_security=?, smtp_username=?, smtp_password_enc=?, tls_verify=? WHERE id=?");
    s.bind(1, i.name).bind(2, i.email).bind(3, i.replyTo).bind(4, i.signature).bind(5, i.useAccountSmtp)
        .bind(6, i.smtpHost).bind(7, i.smtpPort).bind(8, i.smtpSecurity).bind(9, i.smtpUsername)
        .bind(10, encryptOptional(i.smtpPassword))
        .bind(11, i.tlsVerify).bind(12, i.id);
    if (s.run() != SQLITE_DONE) {
        throw std::runtime_error(std::string("Kunde inte uppdatera avsändaren: ") + sqlite3_errmsg(db_));
    }
}

void Database::deleteIdentity(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM identities WHERE id = ?").bind(1, id).run();
}

}  // namespace brevduva
