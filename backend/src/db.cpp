#include "db.hpp"

#include "crypto.hpp"
#include "errors.hpp"

#include "db_stmt.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace brevduva {

namespace {

using Lock = std::lock_guard<std::recursive_mutex>;

// Version 1: användare, sessioner, e-postkonton (med OAuth) och avsändaridentiteter.
// Version 2: Google Drive-kopplingar, backupscheman och backupkörningar.
// Version 3: inställningar för AI-sortering.
// Version 4: automatisk AI-sortering med historik.
// Version 5: formaterade signaturer (signature_html på konton och avsändare).
// Version 6: mejlcache (mappar, rubriker och meddelandetext).
// Version 13: ändringsnummer (modseq) och gravstenar i mejlcachen, för Android-appens synk av ändringar.
// Version 14: enheter som får push-notiser (Android-appen).
constexpr int kSchemaVersion = 16;

// MariaDB kör en sats per anrop, så tabellerna skapas var för sig.
const char* const kSchema[] = {
    R"SQL(CREATE TABLE IF NOT EXISTS schema_version (
    version INT NOT NULL
) ENGINE=InnoDB)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS users (
    id            BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(64) NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    is_admin      TINYINT(1) NOT NULL DEFAULT 0,
    trash_days    INT NOT NULL DEFAULT 30,
    created_at    BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP())
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS sessions (
    token_hash VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    user_id    BIGINT NOT NULL,
    expires_at BIGINT NOT NULL,
    INDEX sessions_user (user_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS accounts (
    id              BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id         BIGINT NOT NULL,
    name            TEXT NOT NULL,
    email           TEXT NOT NULL,
    imap_host       TEXT NOT NULL,
    imap_port       INT NOT NULL,
    imap_security   VARCHAR(16) NOT NULL,
    smtp_host       TEXT NOT NULL,
    smtp_port       INT NOT NULL,
    smtp_security   VARCHAR(16) NOT NULL,
    username        TEXT NOT NULL,
    smtp_username   TEXT NOT NULL DEFAULT '',
    password_enc    TEXT NOT NULL,
    tls_verify      TINYINT(1) NOT NULL DEFAULT 1,
    signature       MEDIUMTEXT NOT NULL DEFAULT '',
    signature_html  MEDIUMTEXT NOT NULL DEFAULT '',
    auth_type       VARCHAR(32) NOT NULL DEFAULT 'password',
    oauth_token_enc TEXT NOT NULL DEFAULT '',
    created_at      BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    INDEX accounts_user (user_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS identities (
    id                BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    account_id        BIGINT NOT NULL,
    name              TEXT NOT NULL,
    email             TEXT NOT NULL,
    reply_to          TEXT NOT NULL DEFAULT '',
    signature         MEDIUMTEXT NOT NULL DEFAULT '',
    signature_html    MEDIUMTEXT NOT NULL DEFAULT '',
    use_account_smtp  TINYINT(1) NOT NULL DEFAULT 1,
    smtp_host         TEXT NOT NULL DEFAULT '',
    smtp_port         INT NOT NULL DEFAULT 587,
    smtp_security     VARCHAR(16) NOT NULL DEFAULT 'starttls',
    smtp_username     TEXT NOT NULL DEFAULT '',
    smtp_password_enc TEXT NOT NULL DEFAULT '',
    tls_verify        TINYINT(1) NOT NULL DEFAULT 1,
    created_at        BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    INDEX identities_account (account_id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS drive_connections (
    id                BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id           BIGINT NOT NULL,
    email             TEXT NOT NULL,
    refresh_token_enc TEXT NOT NULL,
    folder_id         VARCHAR(128) NOT NULL DEFAULT '',
    created_at        BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    INDEX drive_connections_user (user_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS backup_jobs (
    id          BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT NOT NULL,
    name        TEXT NOT NULL,
    config      MEDIUMTEXT NOT NULL,
    schedule    TEXT NOT NULL,
    enabled     TINYINT(1) NOT NULL DEFAULT 1,
    next_run_at BIGINT NOT NULL DEFAULT 0,
    created_at  BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    INDEX backup_jobs_user (user_id),
    INDEX backup_jobs_due (enabled, next_run_at),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS backup_runs (
    id          BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT NOT NULL,
    job_id      BIGINT NULL,
    status      VARCHAR(16) NOT NULL,
    config      MEDIUMTEXT NOT NULL,
    progress    MEDIUMTEXT NOT NULL DEFAULT '',
    message     TEXT NOT NULL DEFAULT '',
    file_path   TEXT NOT NULL DEFAULT '',
    file_size   BIGINT NOT NULL DEFAULT 0,
    created_at  BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    started_at  BIGINT NOT NULL DEFAULT 0,
    finished_at BIGINT NOT NULL DEFAULT 0,
    expires_at  BIGINT NOT NULL DEFAULT 0,
    INDEX backup_runs_user (user_id, id),
    INDEX backup_runs_status (status),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (job_id) REFERENCES backup_jobs(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS ai_settings (
    user_id      BIGINT NOT NULL PRIMARY KEY,
    base_url     TEXT NOT NULL,
    model        VARCHAR(200) NOT NULL,
    api_key_enc  TEXT NOT NULL DEFAULT '',
    instructions MEDIUMTEXT NOT NULL DEFAULT '',
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS cache_folders (
    id                  BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    account_id          BIGINT NOT NULL,
    path_hash           CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    path                TEXT NOT NULL,
    delim               VARCHAR(8) NOT NULL DEFAULT '',
    special_use         VARCHAR(16) NOT NULL DEFAULT '',
    selectable          TINYINT(1) NOT NULL DEFAULT 1,
    sort_order          INT NOT NULL DEFAULT 0,
    total               BIGINT NOT NULL DEFAULT 0,
    unread              BIGINT NOT NULL DEFAULT 0,
    uid_validity        BIGINT NOT NULL DEFAULT 0,
    uid_next            BIGINT NOT NULL DEFAULT 0,
    synced_uid_validity BIGINT NOT NULL DEFAULT 0,
    synced_uid_next     BIGINT NOT NULL DEFAULT 0,
    synced_total        BIGINT NOT NULL DEFAULT -1,
    synced_unread       BIGINT NOT NULL DEFAULT -1,
    headers_complete    TINYINT(1) NOT NULL DEFAULT 0,
    dirty_at            BIGINT NOT NULL DEFAULT 0,
    reconciled_at       BIGINT NOT NULL DEFAULT 0,
    reset_modseq        BIGINT NOT NULL DEFAULT 0,
    UNIQUE KEY cache_folders_path (account_id, path_hash),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS cache_messages (
    id              BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    folder_id       BIGINT NOT NULL,
    uid             BIGINT NOT NULL,
    subject         TEXT NOT NULL,
    sender          TEXT NOT NULL,
    recipients      MEDIUMTEXT NOT NULL,
    date            BIGINT NOT NULL DEFAULT 0,
    size            BIGINT NOT NULL DEFAULT 0,
    seen            TINYINT(1) NOT NULL DEFAULT 0,
    flagged         TINYINT(1) NOT NULL DEFAULT 0,
    answered        TINYINT(1) NOT NULL DEFAULT 0,
    draft           TINYINT(1) NOT NULL DEFAULT 0,
    has_attachments TINYINT(1) NOT NULL DEFAULT 0,
    body_state      TINYINT NOT NULL DEFAULT 0,
    modseq          BIGINT NOT NULL DEFAULT 0,
    UNIQUE KEY cache_messages_uid (folder_id, uid),
    INDEX cache_messages_body (folder_id, body_state, uid),
    INDEX cache_messages_date (folder_id, date, uid),
    INDEX cache_messages_modseq (modseq),
    FOREIGN KEY (folder_id) REFERENCES cache_folders(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    // Ändringsräknaren för mejlcachen (en enda rad, id = 1). Se CacheSeq i db.hpp.
    R"SQL(CREATE TABLE IF NOT EXISTS cache_seq (
    id            TINYINT NOT NULL PRIMARY KEY,
    value         BIGINT NOT NULL DEFAULT 0,
    pruned_modseq BIGINT NOT NULL DEFAULT 0
) ENGINE=InnoDB)SQL",

    // Borttagna meddelanden, så att en klient som följer ändringarna får veta vad som försvunnit.
    R"SQL(CREATE TABLE IF NOT EXISTS cache_tombstones (
    id         BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    folder_id  BIGINT NOT NULL,
    uid        BIGINT NOT NULL,
    modseq     BIGINT NOT NULL,
    created_at BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    INDEX cache_tombstones_modseq (modseq),
    INDEX cache_tombstones_folder (folder_id, modseq),
    FOREIGN KEY (folder_id) REFERENCES cache_folders(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    // Texten ligger i en egen tabell, så att listor inte behöver läsa förbi den. text_z och html_z är zlib-komprimerade;
    // search_text är okomprimerad ren text för fulltextsökning.
    R"SQL(CREATE TABLE IF NOT EXISTS cache_bodies (
    message_id  BIGINT NOT NULL PRIMARY KEY,
    meta        MEDIUMTEXT NOT NULL,
    text_z      MEDIUMBLOB NOT NULL,
    html_z      MEDIUMBLOB NOT NULL,
    search_text MEDIUMTEXT NOT NULL,
    FULLTEXT KEY cache_bodies_search (search_text),
    FOREIGN KEY (message_id) REFERENCES cache_messages(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS ai_auto_rules (
    account_id       BIGINT NOT NULL PRIMARY KEY,
    user_id          BIGINT NOT NULL,
    enabled          TINYINT(1) NOT NULL DEFAULT 0,
    targets          MEDIUMTEXT NOT NULL,
    interval_minutes INT NOT NULL DEFAULT 5,
    uid_validity     BIGINT NOT NULL DEFAULT 0,
    last_uid         BIGINT NOT NULL DEFAULT 0,
    next_check_at    BIGINT NOT NULL DEFAULT 0,
    last_run_at      BIGINT NOT NULL DEFAULT 0,
    last_error       TEXT NOT NULL DEFAULT '',
    INDEX ai_auto_rules_due (enabled, next_check_at),
    INDEX ai_auto_rules_user (user_id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS ai_sort_prefs (
    account_id BIGINT NOT NULL PRIMARY KEY,
    user_id    BIGINT NOT NULL,
    excluded   MEDIUMTEXT NOT NULL,
    cursors    MEDIUMTEXT NOT NULL DEFAULT '',
    INDEX ai_sort_prefs_user (user_id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS ai_sort_log (
    id            BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       BIGINT NOT NULL,
    account_id    BIGINT NOT NULL,
    message_hash  CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    message_id    TEXT NOT NULL,
    subject       TEXT NOT NULL,
    sender        TEXT NOT NULL,
    source_folder TEXT NOT NULL,
    target_folder TEXT NOT NULL,
    reason        TEXT NOT NULL,
    moved_at      BIGINT NOT NULL,
    undone_at     BIGINT NOT NULL DEFAULT 0,
    job_id        BIGINT NOT NULL DEFAULT 0,
    rule_id       BIGINT NOT NULL DEFAULT 0,
    INDEX ai_sort_log_job (job_id, id),
    INDEX ai_sort_log_user (user_id, id),
    INDEX ai_sort_log_message (account_id, message_hash),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    // API-nyckel och modell per AI-tjänst. ai_settings pekar ut den tjänst som används.
    R"SQL(CREATE TABLE IF NOT EXISTS ai_provider_keys (
    user_id     BIGINT NOT NULL,
    url_hash    CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    base_url    TEXT NOT NULL,
    model       VARCHAR(200) NOT NULL,
    api_key_enc TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (user_id, url_hash),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS remote_senders (
    id          BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT NOT NULL,
    sender_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    sender      TEXT NOT NULL,
    created_at  BIGINT NOT NULL DEFAULT 0,
    UNIQUE KEY remote_senders_sender (user_id, sender_hash),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    // Telefoner med Android-appen. Token lagras i klartext (den behövs för utskicket) och hashad för den unika nyckeln,
    // eftersom Google inte lovar någon största längd.
    R"SQL(CREATE TABLE IF NOT EXISTS devices (
    id           BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id      BIGINT NOT NULL,
    token_hash   CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    token        TEXT NOT NULL,
    name         VARCHAR(100) NOT NULL DEFAULT '',
    platform     VARCHAR(16) NOT NULL DEFAULT 'android',
    app_version  VARCHAR(32) NOT NULL DEFAULT '',
    notify       MEDIUMTEXT NOT NULL,
    created_at   BIGINT NOT NULL DEFAULT (UNIX_TIMESTAMP()),
    last_seen_at BIGINT NOT NULL DEFAULT 0,
    UNIQUE KEY devices_token (token_hash),
    INDEX devices_user (user_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    // Första gången varje meddelande sågs i kontots papperskorg, så att det kan tas bort efter users.trash_days
    // dagar. IMAP ger ingen tid för när ett meddelande flyttades, bara dess ursprungliga datum.
    R"SQL(CREATE TABLE IF NOT EXISTS trash_seen (
    account_id   BIGINT NOT NULL,
    uid_validity INT UNSIGNED NOT NULL,
    uid          INT UNSIGNED NOT NULL,
    first_seen   BIGINT NOT NULL,
    PRIMARY KEY (account_id, uid_validity, uid),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS ai_sort_examples (
    id            BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       BIGINT NOT NULL,
    account_id    BIGINT NOT NULL,
    sender_hash   CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    sender_email  TEXT NOT NULL,
    sender_name   TEXT NOT NULL,
    subject       TEXT NOT NULL,
    source_folder TEXT NOT NULL,
    suggested     TEXT NOT NULL,
    chosen        TEXT NOT NULL,
    corrected     TINYINT(1) NOT NULL DEFAULT 0,
    created_at    BIGINT NOT NULL DEFAULT 0,
    INDEX ai_sort_examples_sender (account_id, sender_hash, id),
    INDEX ai_sort_examples_corrected (account_id, corrected, id),
    INDEX ai_sort_examples_user (user_id, id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS mail_rules (
    id         BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id    BIGINT NOT NULL,
    account_id BIGINT NOT NULL,
    name       TEXT NOT NULL,
    enabled    TINYINT(1) NOT NULL DEFAULT 1,
    position   INT NOT NULL DEFAULT 0,
    match_all  TINYINT(1) NOT NULL DEFAULT 1,
    conditions MEDIUMTEXT NOT NULL,
    actions    MEDIUMTEXT NOT NULL,
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0,
    INDEX mail_rules_account (account_id, position, id),
    INDEX mail_rules_user (user_id, id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS rule_state (
    account_id    BIGINT NOT NULL PRIMARY KEY,
    uid_validity  BIGINT NOT NULL DEFAULT 0,
    last_uid      BIGINT NOT NULL DEFAULT 0,
    next_check_at BIGINT NOT NULL DEFAULT 0,
    last_run_at   BIGINT NOT NULL DEFAULT 0,
    last_error    TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS rule_dismissed (
    id          BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT NOT NULL,
    account_id  BIGINT NOT NULL,
    sender_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    sender      TEXT NOT NULL,
    created_at  BIGINT NOT NULL DEFAULT 0,
    UNIQUE KEY rule_dismissed_sender (account_id, sender_hash),
    INDEX rule_dismissed_user (user_id, id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",

    R"SQL(CREATE TABLE IF NOT EXISTS ai_sort_jobs (
    id           BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id      BIGINT NOT NULL,
    account_id   BIGINT NOT NULL,
    folder       TEXT NOT NULL,
    targets      MEDIUMTEXT NOT NULL,
    unread_only  TINYINT(1) NOT NULL DEFAULT 0,
    state        VARCHAR(16) NOT NULL DEFAULT 'running',
    before_uid   BIGINT NOT NULL DEFAULT 0,
    total        BIGINT NOT NULL DEFAULT 0,
    processed    BIGINT NOT NULL DEFAULT 0,
    moved        BIGINT NOT NULL DEFAULT 0,
    undo_cursor  BIGINT NOT NULL DEFAULT 0,
    failures     INT NOT NULL DEFAULT 0,
    last_error   TEXT NOT NULL DEFAULT '',
    next_run_at  BIGINT NOT NULL DEFAULT 0,
    created_at   BIGINT NOT NULL DEFAULT 0,
    finished_at  BIGINT NOT NULL DEFAULT 0,
    INDEX ai_sort_jobs_due (state, next_run_at),
    INDEX ai_sort_jobs_user (user_id, id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL",
};

const char* kIdentityColumns =
    "id, account_id, name, email, reply_to, signature, use_account_smtp, smtp_host, smtp_port, smtp_security, "
    "smtp_username, smtp_password_enc, tls_verify, signature_html";

const char* kAccountColumns =
    "id, user_id, name, email, imap_host, imap_port, imap_security, smtp_host, smtp_port, smtp_security, "
    "username, smtp_username, password_enc, tls_verify, signature, auth_type, oauth_token_enc, signature_html";

const char* kUserColumns = "id, username, is_admin, trash_days, created_at";

const char* kDriveColumns = "id, user_id, email, refresh_token_enc, folder_id, created_at";

const char* kJobColumns = "id, user_id, name, config, schedule, enabled, next_run_at, created_at";

const char* kRunColumns =
    "id, user_id, job_id, status, config, progress, message, file_path, file_size, created_at, started_at, "
    "finished_at, expires_at";

const char* kRuleColumns =
    "id, user_id, account_id, name, enabled, position, match_all, conditions, actions, created_at, updated_at";

const char* kRuleStateColumns = "account_id, uid_validity, last_uid, next_check_at, last_run_at, last_error";

const char* kAutoRuleColumns =
    "account_id, user_id, enabled, targets, interval_minutes, uid_validity, last_uid, next_check_at, last_run_at, "
    "last_error";

const char* kSortLogColumns =
    "id, user_id, account_id, message_id, subject, sender, source_folder, target_folder, reason, moved_at, undone_at, "
    "job_id, rule_id";

const char* kSortJobColumns =
    "id, user_id, account_id, folder, targets, unread_only, state, before_uid, total, processed, moved, undo_cursor, "
    "failures, last_error, next_run_at, created_at, finished_at";

constexpr int kConnectAttempts = 30;
constexpr auto kConnectRetryDelay = std::chrono::seconds(2);


}  // namespace

Database::Database(const Config& config, bool migrateSchema) : key_(config.encryptionKey) {
    std::string where = config.dbHost + ":" + std::to_string(config.dbPort);
    // Databasen kan starta långsammare än appen, till exempel första gången i Docker.
    for (int attempt = 1;; ++attempt) {
        db_ = mysql_init(nullptr);
        if (!db_) throw std::runtime_error("Could not initialize the MariaDB client");
        my_bool reconnect = 1;
        unsigned int timeout = 10;
        mysql_options(db_, MYSQL_OPT_RECONNECT, &reconnect);
        mysql_options(db_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_options(db_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (mysql_real_connect(db_, config.dbHost.c_str(), config.dbUser.c_str(), config.dbPassword.c_str(),
                               config.dbName.c_str(), static_cast<unsigned int>(config.dbPort), nullptr, 0)) {
            break;
        }
        std::string err = mysql_error(db_);
        mysql_close(db_);
        db_ = nullptr;
        if (attempt >= kConnectAttempts) {
            throw std::runtime_error("Could not connect to MariaDB at " + where + ": " + err);
        }
        std::cerr << "[db] Waiting for MariaDB at " << where << ": " << err << std::endl;
        std::this_thread::sleep_for(kConnectRetryDelay);
    }
    if (!migrateSchema) return;
    try {
        for (const char* sql : kSchema) exec(sql);
        exec("INSERT IGNORE INTO cache_seq (id, value) VALUES (1, 0)");
        migrate();
    } catch (...) {
        mysql_close(db_);
        throw;
    }
    std::cout << "[db] Connected to MariaDB at " << where << "/" << config.dbName << std::endl;
}

Database::~Database() {
    if (db_) mysql_close(db_);
}

void Database::exec(const std::string& sql) {
    Lock lock(mutex_);
    if (mysql_real_query(db_, sql.data(), sql.size()) == 0) return;
    if (connectionLost(mysql_errno(db_)) && mysql_ping(db_) == 0 &&
        mysql_real_query(db_, sql.data(), sql.size()) == 0) {
        return;
    }
    throw std::runtime_error("SQL error: " + std::string(mysql_error(db_)));
}

void Database::migrate() {
    Lock lock(mutex_);
    Stmt version(db_, "SELECT MAX(version) FROM schema_version");
    version.row();
    int current = version.int32(0);
    if (current >= kSchemaVersion) return;

    // Version 2, 3, 4, 6, 10 och 11 lade bara till tabeller, som skapas av kSchema ovan.
    if (current < 5) {
        // En ny databas har redan kolumnerna från kSchema; då gör IF NOT EXISTS satserna till tomma operationer.
        exec("ALTER TABLE accounts ADD COLUMN IF NOT EXISTS signature_html MEDIUMTEXT NOT NULL DEFAULT '' AFTER signature");
        exec("ALTER TABLE identities ADD COLUMN IF NOT EXISTS signature_html MEDIUMTEXT NOT NULL DEFAULT '' AFTER signature");
    }
    if (current < 7) {
        exec("ALTER TABLE ai_sort_prefs ADD COLUMN IF NOT EXISTS cursors MEDIUMTEXT NOT NULL DEFAULT '' AFTER excluded");
    }
    if (current < 8) {
        exec("ALTER TABLE ai_sort_log ADD COLUMN IF NOT EXISTS job_id BIGINT NOT NULL DEFAULT 0 AFTER undone_at");
        exec("ALTER TABLE ai_sort_log ADD INDEX IF NOT EXISTS ai_sort_log_job (job_id, id)");
    }
    if (current < 12) {
        // Listorna sorteras efter datum från början, så mappens meddelanden ska gå att läsa i datumordning via index.
        exec("ALTER TABLE cache_messages ADD INDEX IF NOT EXISTS cache_messages_date (folder_id, date, uid)");
    }
    if (current < 13) {
        // Befintliga rader behåller modseq 0. En klient börjar alltid från det aktuella numret, så inget behöver
        // fyllas i bakåt.
        exec("ALTER TABLE cache_messages ADD COLUMN IF NOT EXISTS modseq BIGINT NOT NULL DEFAULT 0 AFTER body_state");
        exec("ALTER TABLE cache_messages ADD INDEX IF NOT EXISTS cache_messages_modseq (modseq)");
        exec("ALTER TABLE cache_folders ADD COLUMN IF NOT EXISTS reset_modseq BIGINT NOT NULL DEFAULT 0 AFTER reconciled_at");
    }
    if (current < 15) {
        // Tömning av papperskorgen efter ett antal dagar (0 = aldrig). Tabellen trash_seen skapas av kSchema.
        exec("ALTER TABLE users ADD COLUMN IF NOT EXISTS trash_days INT NOT NULL DEFAULT 30 AFTER is_admin");
    }
    if (current < 16) {
        // Regler för inkommande post ersätter AI-sorteringens avsändarregler. De gamla reglerna blir regler med ett
        // villkor på avsändaren (adress eller domän) och flytt till samma mapp; "ligger kvar" blir en regel som bara
        // stoppar. Nej tack till ett förslag flyttas till rule_dismissed. Tabellen skapas först om den saknas, så att
        // en ny databas också går igenom steget.
        exec("CREATE TABLE IF NOT EXISTS ai_sender_rules (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, user_id BIGINT "
             "NOT NULL, account_id BIGINT NOT NULL, sender_hash CHAR(64) NOT NULL, sender TEXT NOT NULL, target TEXT NOT "
             "NULL, dismissed TINYINT(1) NOT NULL DEFAULT 0, created_at BIGINT NOT NULL DEFAULT 0)");
        exec("INSERT INTO mail_rules (user_id, account_id, name, enabled, position, match_all, conditions, actions, "
             "created_at, updated_at) SELECT user_id, account_id, sender, 1, id, 1, "
             "CONCAT('[{\"field\":\"from\",\"op\":\"', IF(LEFT(sender, 1) = '@', 'domain', 'is'), '\",\"value\":', "
             "JSON_QUOTE(sender), '}]'), "
             "IF(target = '', '{\"stop\":true}', CONCAT('{\"move\":', JSON_QUOTE(target), ',\"stop\":true}')), "
             "created_at, created_at FROM ai_sender_rules WHERE dismissed = 0 ORDER BY id");
        exec("INSERT IGNORE INTO rule_dismissed (user_id, account_id, sender_hash, sender, created_at) "
             "SELECT user_id, account_id, sender_hash, sender, created_at FROM ai_sender_rules WHERE dismissed = 1");
        exec("DROP TABLE ai_sender_rules");
        exec("ALTER TABLE ai_sort_log ADD COLUMN IF NOT EXISTS rule_id BIGINT NOT NULL DEFAULT 0 AFTER job_id");
    }
    // Framtida schemaändringar läggs här: if (current < 3) { ... }
    exec("DELETE FROM schema_version");
    Stmt(db_, "INSERT INTO schema_version (version) VALUES (?)").bind(1, kSchemaVersion).run();
}

// Tomma hemligheter lagras som tom sträng i stället för chiffertext.
std::string Database::encryptOptional(const std::string& plaintext) {
    return plaintext.empty() ? std::string() : crypto::encrypt(key_, plaintext);
}

std::string Database::decryptOptional(const std::string& token) {
    return token.empty() ? std::string() : crypto::decrypt(key_, token);
}

// --- Användare ----------------------------------------------------------------

User Database::rowToUser(Stmt& s) {
    User u;
    u.id = s.int64(0);
    u.username = s.text(1);
    u.isAdmin = s.int32(2) != 0;
    u.trashDays = s.int32(3);
    u.createdAt = s.int64(4);
    return u;
}

int Database::countUsers() {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM users");
    s.row();
    return s.int32(0);
}

int Database::countAdmins() {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM users WHERE is_admin = 1");
    s.row();
    return s.int32(0);
}

std::vector<User> Database::listUsers() {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kUserColumns + " FROM users ORDER BY username");
    std::vector<User> out;
    while (s.row()) out.push_back(rowToUser(s));
    return out;
}

std::optional<User> Database::getUser(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kUserColumns + " FROM users WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToUser(s);
    return std::nullopt;
}

std::optional<std::pair<User, std::string>> Database::findUserForLogin(const std::string& username) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kUserColumns + ", password_hash FROM users WHERE username = ?");
    s.bind(1, username);
    if (s.row()) return std::make_pair(rowToUser(s), s.text(5));
    return std::nullopt;
}

long long Database::createUser(const std::string& username, const std::string& passwordHash, bool isAdmin) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO users (username, password_hash, is_admin) VALUES (?, ?, ?)");
    s.bind(1, username).bind(2, passwordHash).bind(3, isAdmin);
    if (!s.run()) throw BadRequest("That username is already taken");
    return s.insertId();
}

std::optional<long long> Database::createFirstAdmin(const std::string& username, const std::string& passwordHash) {
    Lock lock(mutex_);
    exec("START TRANSACTION");
    try {
        // FOR UPDATE låser tabellen mot samtidiga inlägg tills transaktionen är klar.
        Stmt count(db_, "SELECT COUNT(*) FROM users FOR UPDATE");
        count.row();
        if (count.int32(0) > 0) {
            exec("ROLLBACK");
            return std::nullopt;
        }
        long long id = createUser(username, passwordHash, true);
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

void Database::setUserTrashDays(long long id, int days) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE users SET trash_days = ? WHERE id = ?");
    s.bind(1, days).bind(2, id).run();
}

void Database::deleteUser(long long id) {
    Lock lock(mutex_);
    // Sessioner, konton och kontonas avsändare tas bort via ON DELETE CASCADE.
    Stmt(db_, "DELETE FROM users WHERE id = ?").bind(1, id).run();
}

// --- Enheter för push-notiser -------------------------------------------------

namespace {

const char* kDeviceColumns = "id, user_id, token, name, platform, app_version, notify, created_at, last_seen_at";

Device rowToDevice(Stmt& s) {
    Device d;
    d.id = s.int64(0);
    d.userId = s.int64(1);
    d.token = s.text(2);
    d.name = s.text(3);
    d.platform = s.text(4);
    d.appVersion = s.text(5);
    d.notify = s.text(6);
    d.createdAt = s.int64(7);
    d.lastSeenAt = s.int64(8);
    return d;
}

std::string tokenHash(const std::string& token) {
    return crypto::toHex(crypto::sha256(token));
}

}  // namespace

std::vector<Device> Database::listDevices(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kDeviceColumns + " FROM devices WHERE user_id = ? ORDER BY id");
    s.bind(1, userId);
    std::vector<Device> out;
    while (s.row()) out.push_back(rowToDevice(s));
    return out;
}

void Database::upsertDevice(const Device& d) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO devices (user_id, token_hash, token, name, platform, app_version, notify, created_at, "
                "last_seen_at) VALUES (?,?,?,?,?,?,?,UNIX_TIMESTAMP(),UNIX_TIMESTAMP()) "
                // Fält som inte skickas med (tomma) behåller sitt värde, så att appen kan uppdatera t.ex. bara mappvalet.
                "ON DUPLICATE KEY UPDATE user_id = VALUES(user_id), name = IF(VALUES(name) = '', name, VALUES(name)), "
                "platform = IF(VALUES(platform) = '', platform, VALUES(platform)), "
                "app_version = IF(VALUES(app_version) = '', app_version, VALUES(app_version)), "
                "notify = IF(VALUES(notify) = '', notify, VALUES(notify)), last_seen_at = UNIX_TIMESTAMP()");
    s.bind(1, d.userId).bind(2, tokenHash(d.token)).bind(3, d.token).bind(4, d.name).bind(5, d.platform)
        .bind(6, d.appVersion).bind(7, d.notify);
    if (!s.run()) throw std::runtime_error("Could not save the device");
}

bool Database::deleteDevice(long long userId, const std::string& token) {
    Lock lock(mutex_);
    Stmt s(db_, "DELETE FROM devices WHERE user_id = ? AND token_hash = ?");
    s.bind(1, userId).bind(2, tokenHash(token)).run();
    return s.affectedRows() > 0;
}

void Database::deleteDeviceByToken(const std::string& token) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM devices WHERE token_hash = ?").bind(1, tokenHash(token)).run();
}

// --- Papperskorgen -------------------------------------------------------------

std::map<std::uint32_t, std::int64_t> Database::trashSeen(long long accountId, std::uint32_t uidValidity) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT uid, first_seen FROM trash_seen WHERE account_id = ? AND uid_validity = ?");
    s.bind(1, accountId).bind(2, static_cast<long long>(uidValidity));
    std::map<std::uint32_t, std::int64_t> out;
    while (s.row()) out[static_cast<std::uint32_t>(s.int64(0))] = s.int64(1);
    return out;
}

void Database::recordTrashSeen(long long accountId, std::uint32_t uidValidity, const std::vector<std::uint32_t>& uids,
                               std::int64_t seenAt) {
    Lock lock(mutex_);
    writeTx([&] {
        Stmt(db_, "DELETE FROM trash_seen WHERE account_id = ? AND uid_validity <> ?")
            .bind(1, accountId).bind(2, static_cast<long long>(uidValidity)).run();
        for (auto uid : uids) {
            Stmt(db_, "INSERT IGNORE INTO trash_seen (account_id, uid_validity, uid, first_seen) VALUES (?, ?, ?, ?)")
                .bind(1, accountId).bind(2, static_cast<long long>(uidValidity)).bind(3, static_cast<long long>(uid))
                .bind(4, static_cast<long long>(seenAt)).run();
        }
    });
}

void Database::deleteTrashSeen(long long accountId, std::uint32_t uidValidity, const std::vector<std::uint32_t>& uids) {
    Lock lock(mutex_);
    writeTx([&] {
        for (auto uid : uids) {
            Stmt(db_, "DELETE FROM trash_seen WHERE account_id = ? AND uid_validity = ? AND uid = ?")
                .bind(1, accountId).bind(2, static_cast<long long>(uidValidity)).bind(3, static_cast<long long>(uid)).run();
        }
    });
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
           "SELECT u.id, u.username, u.is_admin, u.trash_days, u.created_at FROM sessions s JOIN users u ON u.id = s.user_id "
           "WHERE s.token_hash = ? AND s.expires_at > ?");
    s.bind(1, tokenHash).bind(2, static_cast<long long>(now));
    if (s.row()) return rowToUser(s);
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

std::int64_t Database::sessionExpiresAt(const std::string& tokenHash) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT expires_at FROM sessions WHERE token_hash = ?");
    s.bind(1, tokenHash);
    return s.row() ? s.int64(0) : 0;
}

void Database::purgeExpiredSessions(std::int64_t now) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM sessions WHERE expires_at <= ?").bind(1, static_cast<long long>(now)).run();
}

// --- E-postkonton -------------------------------------------------------------

Account Database::rowToAccount(Stmt& s) {
    Account a;
    a.id = s.int64(0);
    a.userId = s.int64(1);
    a.name = s.text(2);
    a.email = s.text(3);
    a.imapHost = s.text(4);
    a.imapPort = s.int32(5);
    a.imapSecurity = s.text(6);
    a.smtpHost = s.text(7);
    a.smtpPort = s.int32(8);
    a.smtpSecurity = s.text(9);
    a.username = s.text(10);
    a.smtpUsername = s.text(11);
    a.password = decryptOptional(s.text(12));
    a.tlsVerify = s.int32(13) != 0;
    a.signature = s.text(14);
    a.authType = s.text(15);
    a.oauthRefreshToken = decryptOptional(s.text(16));
    a.signatureHtml = s.text(17);
    return a;
}

std::vector<Account> Database::listAccounts(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kAccountColumns + " FROM accounts WHERE user_id = ? ORDER BY id");
    s.bind(1, userId);
    std::vector<Account> out;
    while (s.row()) out.push_back(rowToAccount(s));
    return out;
}

std::optional<Account> Database::getAccount(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kAccountColumns + " FROM accounts WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToAccount(s);
    return std::nullopt;
}

long long Database::createAccount(const Account& a) {
    Lock lock(mutex_);
    Stmt s(db_,
           "INSERT INTO accounts (user_id, name, email, imap_host, imap_port, imap_security, smtp_host, smtp_port, "
           "smtp_security, username, smtp_username, password_enc, tls_verify, signature, auth_type, oauth_token_enc, "
           "signature_html) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, a.userId).bind(2, a.name).bind(3, a.email).bind(4, a.imapHost).bind(5, a.imapPort)
        .bind(6, a.imapSecurity).bind(7, a.smtpHost).bind(8, a.smtpPort).bind(9, a.smtpSecurity)
        .bind(10, a.username).bind(11, a.smtpUsername).bind(12, encryptOptional(a.password))
        .bind(13, a.tlsVerify).bind(14, a.signature).bind(15, a.authType).bind(16, encryptOptional(a.oauthRefreshToken))
        .bind(17, a.signatureHtml);
    if (!s.run()) throw std::runtime_error("Could not save the account");
    return s.insertId();
}

void Database::updateAccount(const Account& a) {
    Lock lock(mutex_);
    Stmt s(db_,
           "UPDATE accounts SET name=?, email=?, imap_host=?, imap_port=?, imap_security=?, smtp_host=?, "
           "smtp_port=?, smtp_security=?, username=?, smtp_username=?, password_enc=?, tls_verify=?, "
           "signature=?, auth_type=?, oauth_token_enc=?, signature_html=? WHERE id=?");
    s.bind(1, a.name).bind(2, a.email).bind(3, a.imapHost).bind(4, a.imapPort).bind(5, a.imapSecurity)
        .bind(6, a.smtpHost).bind(7, a.smtpPort).bind(8, a.smtpSecurity).bind(9, a.username)
        .bind(10, a.smtpUsername).bind(11, encryptOptional(a.password)).bind(12, a.tlsVerify)
        .bind(13, a.signature).bind(14, a.authType).bind(15, encryptOptional(a.oauthRefreshToken))
        .bind(16, a.signatureHtml).bind(17, a.id);
    if (!s.run()) throw std::runtime_error("Could not update the account");
}

void Database::deleteAccount(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM accounts WHERE id = ?").bind(1, id).run();
}

// --- Avsändaridentiteter --------------------------------------------------------

Identity Database::rowToIdentity(Stmt& s) {
    Identity i;
    i.id = s.int64(0);
    i.accountId = s.int64(1);
    i.name = s.text(2);
    i.email = s.text(3);
    i.replyTo = s.text(4);
    i.signature = s.text(5);
    i.useAccountSmtp = s.int32(6) != 0;
    i.smtpHost = s.text(7);
    i.smtpPort = s.int32(8);
    i.smtpSecurity = s.text(9);
    i.smtpUsername = s.text(10);
    std::string enc = s.text(11);
    i.smtpPassword = decryptOptional(enc);
    i.tlsVerify = s.int32(12) != 0;
    i.signatureHtml = s.text(13);
    return i;
}

std::vector<Identity> Database::listIdentities(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kIdentityColumns + " FROM identities WHERE account_id = ? ORDER BY id");
    s.bind(1, accountId);
    std::vector<Identity> out;
    while (s.row()) out.push_back(rowToIdentity(s));
    return out;
}

std::optional<Identity> Database::getIdentity(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kIdentityColumns + " FROM identities WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToIdentity(s);
    return std::nullopt;
}

long long Database::createIdentity(const Identity& i) {
    Lock lock(mutex_);
    Stmt s(db_,
           "INSERT INTO identities (account_id, name, email, reply_to, signature, use_account_smtp, smtp_host, "
           "smtp_port, smtp_security, smtp_username, smtp_password_enc, tls_verify, signature_html) "
           "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, i.accountId).bind(2, i.name).bind(3, i.email).bind(4, i.replyTo).bind(5, i.signature)
        .bind(6, i.useAccountSmtp).bind(7, i.smtpHost).bind(8, i.smtpPort).bind(9, i.smtpSecurity)
        .bind(10, i.smtpUsername).bind(11, encryptOptional(i.smtpPassword))
        .bind(12, i.tlsVerify).bind(13, i.signatureHtml);
    if (!s.run()) throw std::runtime_error("Could not save the sender");
    return s.insertId();
}

void Database::updateIdentity(const Identity& i) {
    Lock lock(mutex_);
    Stmt s(db_,
           "UPDATE identities SET name=?, email=?, reply_to=?, signature=?, use_account_smtp=?, smtp_host=?, "
           "smtp_port=?, smtp_security=?, smtp_username=?, smtp_password_enc=?, tls_verify=?, signature_html=? "
           "WHERE id=?");
    s.bind(1, i.name).bind(2, i.email).bind(3, i.replyTo).bind(4, i.signature).bind(5, i.useAccountSmtp)
        .bind(6, i.smtpHost).bind(7, i.smtpPort).bind(8, i.smtpSecurity).bind(9, i.smtpUsername)
        .bind(10, encryptOptional(i.smtpPassword))
        .bind(11, i.tlsVerify).bind(12, i.signatureHtml).bind(13, i.id);
    if (!s.run()) throw std::runtime_error("Could not update the sender");
}

void Database::deleteIdentity(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM identities WHERE id = ?").bind(1, id).run();
}

// --- Google Drive-kopplingar ----------------------------------------------------

DriveConnection Database::rowToDriveConnection(Stmt& s) {
    DriveConnection c;
    c.id = s.int64(0);
    c.userId = s.int64(1);
    c.email = s.text(2);
    c.refreshToken = decryptOptional(s.text(3));
    c.folderId = s.text(4);
    c.createdAt = s.int64(5);
    return c;
}

std::vector<DriveConnection> Database::listDriveConnections(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kDriveColumns + " FROM drive_connections WHERE user_id = ? ORDER BY id");
    s.bind(1, userId);
    std::vector<DriveConnection> out;
    while (s.row()) out.push_back(rowToDriveConnection(s));
    return out;
}

std::optional<DriveConnection> Database::getDriveConnection(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kDriveColumns + " FROM drive_connections WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToDriveConnection(s);
    return std::nullopt;
}

long long Database::createDriveConnection(const DriveConnection& c) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO drive_connections (user_id, email, refresh_token_enc, folder_id) VALUES (?,?,?,?)");
    s.bind(1, c.userId).bind(2, c.email).bind(3, encryptOptional(c.refreshToken)).bind(4, c.folderId);
    if (!s.run()) throw std::runtime_error("Could not save the Google Drive connection");
    return s.insertId();
}

void Database::setDriveFolderId(long long id, const std::string& folderId) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE drive_connections SET folder_id = ? WHERE id = ?").bind(1, folderId).bind(2, id).run();
}

void Database::setDriveRefreshToken(long long id, const std::string& refreshToken) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE drive_connections SET refresh_token_enc = ? WHERE id = ?")
        .bind(1, encryptOptional(refreshToken)).bind(2, id).run();
}

void Database::deleteDriveConnection(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM drive_connections WHERE id = ?").bind(1, id).run();
}

// --- Backup -------------------------------------------------------------------

BackupJob Database::rowToBackupJob(Stmt& s) {
    BackupJob j;
    j.id = s.int64(0);
    j.userId = s.int64(1);
    j.name = s.text(2);
    j.config = s.text(3);
    j.schedule = s.text(4);
    j.enabled = s.int32(5) != 0;
    j.nextRunAt = s.int64(6);
    j.createdAt = s.int64(7);
    return j;
}

BackupRun Database::rowToBackupRun(Stmt& s) {
    BackupRun r;
    r.id = s.int64(0);
    r.userId = s.int64(1);
    r.jobId = s.int64(2);
    r.status = s.text(3);
    r.config = s.text(4);
    r.progress = s.text(5);
    r.message = s.text(6);
    r.filePath = s.text(7);
    r.fileSize = s.int64(8);
    r.createdAt = s.int64(9);
    r.startedAt = s.int64(10);
    r.finishedAt = s.int64(11);
    r.expiresAt = s.int64(12);
    return r;
}

std::vector<BackupJob> Database::listBackupJobs(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kJobColumns + " FROM backup_jobs WHERE user_id = ? ORDER BY id");
    s.bind(1, userId);
    std::vector<BackupJob> out;
    while (s.row()) out.push_back(rowToBackupJob(s));
    return out;
}

std::optional<BackupJob> Database::getBackupJob(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kJobColumns + " FROM backup_jobs WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToBackupJob(s);
    return std::nullopt;
}

long long Database::createBackupJob(const BackupJob& j) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO backup_jobs (user_id, name, config, schedule, enabled, next_run_at) VALUES (?,?,?,?,?,?)");
    s.bind(1, j.userId).bind(2, j.name).bind(3, j.config).bind(4, j.schedule).bind(5, j.enabled)
        .bind(6, static_cast<long long>(j.nextRunAt));
    if (!s.run()) throw std::runtime_error("Could not save the backup");
    return s.insertId();
}

void Database::updateBackupJob(const BackupJob& j) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE backup_jobs SET name=?, config=?, schedule=?, enabled=?, next_run_at=? WHERE id=?");
    s.bind(1, j.name).bind(2, j.config).bind(3, j.schedule).bind(4, j.enabled)
        .bind(5, static_cast<long long>(j.nextRunAt)).bind(6, j.id);
    if (!s.run()) throw std::runtime_error("Could not update the backup");
}

void Database::deleteBackupJob(long long id) {
    Lock lock(mutex_);
    // Tidigare körningar finns kvar i historiken (job_id sätts till NULL).
    Stmt(db_, "DELETE FROM backup_jobs WHERE id = ?").bind(1, id).run();
}

std::vector<BackupJob> Database::dueBackupJobs(std::int64_t now) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kJobColumns +
                    " FROM backup_jobs WHERE enabled = 1 AND next_run_at > 0 AND next_run_at <= ? ORDER BY next_run_at");
    s.bind(1, static_cast<long long>(now));
    std::vector<BackupJob> out;
    while (s.row()) out.push_back(rowToBackupJob(s));
    return out;
}

void Database::setBackupJobNextRun(long long id, std::int64_t nextRunAt) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE backup_jobs SET next_run_at = ? WHERE id = ?")
        .bind(1, static_cast<long long>(nextRunAt)).bind(2, id).run();
}

long long Database::createBackupRun(const BackupRun& r) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO backup_runs (user_id, job_id, status, config, progress, message, finished_at) "
                "VALUES (?,?,?,?,?,?,?)");
    s.bind(1, r.userId);
    if (r.jobId) s.bind(2, r.jobId);  // Obunden parameter blir NULL.
    s.bind(3, r.status).bind(4, r.config).bind(5, r.progress).bind(6, r.message)
        .bind(7, static_cast<long long>(r.finishedAt));
    if (!s.run()) throw std::runtime_error("Could not create the backup run");
    return s.insertId();
}

std::optional<BackupRun> Database::getBackupRun(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRunColumns + " FROM backup_runs WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToBackupRun(s);
    return std::nullopt;
}

std::vector<BackupRun> Database::listBackupRuns(long long userId, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRunColumns + " FROM backup_runs WHERE user_id = ? ORDER BY id DESC LIMIT ?");
    s.bind(1, userId).bind(2, limit);
    std::vector<BackupRun> out;
    while (s.row()) out.push_back(rowToBackupRun(s));
    return out;
}

std::optional<BackupRun> Database::lastBackupRun(long long jobId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRunColumns + " FROM backup_runs WHERE job_id = ? ORDER BY id DESC LIMIT 1");
    s.bind(1, jobId);
    if (s.row()) return rowToBackupRun(s);
    return std::nullopt;
}

bool Database::hasActiveBackupRun(long long jobId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM backup_runs WHERE job_id = ? AND status IN ('queued', 'running')");
    s.bind(1, jobId);
    s.row();
    return s.int32(0) > 0;
}

std::optional<BackupRun> Database::claimQueuedBackupRun(std::int64_t now) {
    Lock lock(mutex_);
    std::optional<BackupRun> run;
    {
        Stmt s(db_, std::string("SELECT ") + kRunColumns +
                        " FROM backup_runs WHERE status = 'queued' ORDER BY id LIMIT 1");
        if (s.row()) run = rowToBackupRun(s);
    }
    if (!run) return std::nullopt;
    Stmt(db_, "UPDATE backup_runs SET status = 'running', started_at = ? WHERE id = ?")
        .bind(1, static_cast<long long>(now)).bind(2, run->id).run();
    run->status = "running";
    run->startedAt = now;
    return run;
}

void Database::setBackupRunProgress(long long id, const std::string& progress) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE backup_runs SET progress = ? WHERE id = ?").bind(1, progress).bind(2, id).run();
}

void Database::finishBackupRun(const BackupRun& r) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE backup_runs SET status=?, progress=?, message=?, file_path=?, file_size=?, finished_at=?, "
                "expires_at=? WHERE id=?");
    s.bind(1, r.status).bind(2, r.progress).bind(3, r.message).bind(4, r.filePath).bind(5, r.fileSize)
        .bind(6, static_cast<long long>(r.finishedAt)).bind(7, static_cast<long long>(r.expiresAt)).bind(8, r.id);
    s.run();
}

bool Database::cancelQueuedBackupRun(long long id, std::int64_t now) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE backup_runs SET status = 'cancelled', message = 'Cancelled before it started', finished_at = ? "
                "WHERE id = ? AND status = 'queued'");
    s.bind(1, static_cast<long long>(now)).bind(2, id).run();
    return s.affectedRows() > 0;
}

void Database::deleteBackupRun(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM backup_runs WHERE id = ?").bind(1, id).run();
}

void Database::failInterruptedBackupRuns(std::int64_t now) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE backup_runs SET status = 'failed', message = 'Aborted because the server was restarted', "
              "finished_at = ? WHERE status = 'running'")
        .bind(1, static_cast<long long>(now)).run();
}

std::vector<BackupRun> Database::expiredBackupFiles(std::int64_t now) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRunColumns +
                    " FROM backup_runs WHERE file_path != '' AND expires_at > 0 AND expires_at <= ?");
    s.bind(1, static_cast<long long>(now));
    std::vector<BackupRun> out;
    while (s.row()) out.push_back(rowToBackupRun(s));
    return out;
}

void Database::clearBackupRunFile(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE backup_runs SET file_path = '', expires_at = 0 WHERE id = ?").bind(1, id).run();
}

std::vector<std::string> Database::backupRunFiles() {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT file_path FROM backup_runs WHERE file_path != ''");
    std::vector<std::string> out;
    while (s.row()) out.push_back(s.text(0));
    return out;
}

void Database::pruneBackupRuns(std::int64_t olderThan) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM backup_runs WHERE status IN ('done', 'failed', 'cancelled') AND file_path = '' "
              "AND finished_at > 0 AND finished_at < ?")
        .bind(1, static_cast<long long>(olderThan)).run();
}

// --- AI-sortering -------------------------------------------------------------

std::optional<AiSettings> Database::getAiSettings(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT user_id, base_url, model, api_key_enc, instructions FROM ai_settings WHERE user_id = ?");
    s.bind(1, userId);
    if (!s.row()) return std::nullopt;
    AiSettings a;
    a.userId = s.int64(0);
    a.baseUrl = s.text(1);
    a.model = s.text(2);
    a.apiKey = decryptOptional(s.text(3));
    a.instructions = s.text(4);
    return a;
}

void Database::saveAiSettings(const AiSettings& a) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO ai_settings (user_id, base_url, model, api_key_enc, instructions) VALUES (?,?,?,?,?) "
                "ON DUPLICATE KEY UPDATE base_url = VALUES(base_url), model = VALUES(model), "
                "api_key_enc = VALUES(api_key_enc), instructions = VALUES(instructions)");
    s.bind(1, a.userId).bind(2, a.baseUrl).bind(3, a.model).bind(4, encryptOptional(a.apiKey)).bind(5, a.instructions);
    if (!s.run()) throw std::runtime_error("Could not save the AI settings");
    Stmt k(db_, "INSERT INTO ai_provider_keys (user_id, url_hash, base_url, model, api_key_enc) VALUES (?,?,?,?,?) "
                "ON DUPLICATE KEY UPDATE model = VALUES(model), api_key_enc = VALUES(api_key_enc)");
    k.bind(1, a.userId).bind(2, crypto::toHex(crypto::sha256(a.baseUrl))).bind(3, a.baseUrl).bind(4, a.model)
        .bind(5, encryptOptional(a.apiKey));
    if (!k.run()) throw std::runtime_error("Could not save the AI settings");
}

void Database::deleteAiSettings(long long userId) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM ai_settings WHERE user_id = ?").bind(1, userId).run();
    Stmt(db_, "DELETE FROM ai_provider_keys WHERE user_id = ?").bind(1, userId).run();
}

std::vector<AiSettings> Database::listAiProviderKeys(long long userId) {
    Lock lock(mutex_);
    std::vector<AiSettings> out;
    {
        Stmt s(db_, "SELECT base_url, model, api_key_enc FROM ai_provider_keys WHERE user_id = ?");
        s.bind(1, userId);
        while (s.row()) {
            AiSettings a;
            a.userId = userId;
            a.baseUrl = s.text(0);
            a.model = s.text(1);
            a.apiKey = decryptOptional(s.text(2));
            out.push_back(std::move(a));
        }
    }
    // Inställningar som sparades innan nycklar började sparas per tjänst finns bara som de aktiva.
    auto active = getAiSettings(userId);
    if (active && std::none_of(out.begin(), out.end(), [&](const AiSettings& a) { return a.baseUrl == active->baseUrl; })) {
        out.push_back(*active);
    }
    return out;
}

namespace {

AiSortPrefs rowToAiSortPrefs(Stmt& s) {
    AiSortPrefs p;
    p.accountId = s.int64(0);
    p.userId = s.int64(1);
    p.excluded = s.text(2);
    p.cursors = s.text(3);
    return p;
}

}  // namespace

std::vector<AiSortPrefs> Database::listAiSortPrefs(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT account_id, user_id, excluded, cursors FROM ai_sort_prefs WHERE user_id = ? ORDER BY account_id");
    s.bind(1, userId);
    std::vector<AiSortPrefs> out;
    while (s.row()) out.push_back(rowToAiSortPrefs(s));
    return out;
}

std::optional<AiSortPrefs> Database::getAiSortPrefs(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT account_id, user_id, excluded, cursors FROM ai_sort_prefs WHERE account_id = ?");
    s.bind(1, accountId);
    if (s.row()) return rowToAiSortPrefs(s);
    return std::nullopt;
}

void Database::saveAiSortCursors(long long userId, long long accountId, const std::string& cursors) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO ai_sort_prefs (account_id, user_id, excluded, cursors) VALUES (?,?,'',?) "
                "ON DUPLICATE KEY UPDATE cursors = VALUES(cursors)");
    s.bind(1, accountId).bind(2, userId).bind(3, cursors);
    if (!s.run()) throw std::runtime_error("Could not save the sorting progress");
}

void Database::saveAiSortExcluded(long long userId, long long accountId, const std::string& excluded) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO ai_sort_prefs (account_id, user_id, excluded) VALUES (?,?,?) "
                "ON DUPLICATE KEY UPDATE excluded = VALUES(excluded)");
    s.bind(1, accountId).bind(2, userId).bind(3, excluded);
    if (!s.run()) throw std::runtime_error("Could not save the folder selection");
}

AiAutoRule Database::rowToAiAutoRule(Stmt& s) {
    AiAutoRule r;
    r.accountId = s.int64(0);
    r.userId = s.int64(1);
    r.enabled = s.int32(2) != 0;
    r.targets = s.text(3);
    r.intervalMinutes = s.int32(4);
    r.uidValidity = s.int64(5);
    r.lastUid = s.int64(6);
    r.nextCheckAt = s.int64(7);
    r.lastRunAt = s.int64(8);
    r.lastError = s.text(9);
    return r;
}

std::optional<AiAutoRule> Database::getAiAutoRule(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kAutoRuleColumns + " FROM ai_auto_rules WHERE account_id = ?");
    s.bind(1, accountId);
    if (s.row()) return rowToAiAutoRule(s);
    return std::nullopt;
}

std::vector<AiAutoRule> Database::listAiAutoRules(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kAutoRuleColumns + " FROM ai_auto_rules WHERE user_id = ? ORDER BY account_id");
    s.bind(1, userId);
    std::vector<AiAutoRule> out;
    while (s.row()) out.push_back(rowToAiAutoRule(s));
    return out;
}

void Database::saveAiAutoRule(const AiAutoRule& r, bool resetState) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO ai_auto_rules (account_id, user_id, enabled, targets, interval_minutes) VALUES (?,?,?,?,?) "
                "ON DUPLICATE KEY UPDATE enabled = VALUES(enabled), targets = VALUES(targets), "
                "interval_minutes = VALUES(interval_minutes), next_check_at = 0");
    s.bind(1, r.accountId).bind(2, r.userId).bind(3, r.enabled).bind(4, r.targets).bind(5, r.intervalMinutes);
    if (!s.run()) throw std::runtime_error("Could not save the automatic sorting");
    if (resetState) {
        Stmt(db_, "UPDATE ai_auto_rules SET uid_validity = 0, last_uid = 0, last_error = '' WHERE account_id = ?")
            .bind(1, r.accountId).run();
    }
}

void Database::setAiAutoRuleState(const AiAutoRule& r) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE ai_auto_rules SET uid_validity = ?, last_uid = ?, next_check_at = ?, last_run_at = ?, "
                "last_error = ? WHERE account_id = ?");
    s.bind(1, static_cast<long long>(r.uidValidity)).bind(2, static_cast<long long>(r.lastUid))
        .bind(3, static_cast<long long>(r.nextCheckAt)).bind(4, static_cast<long long>(r.lastRunAt))
        .bind(5, r.lastError).bind(6, r.accountId).run();
}

std::vector<AiAutoRule> Database::dueAiAutoRules(std::int64_t now) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kAutoRuleColumns +
                    " FROM ai_auto_rules WHERE enabled = 1 AND next_check_at <= ? ORDER BY next_check_at");
    s.bind(1, static_cast<long long>(now));
    std::vector<AiAutoRule> out;
    while (s.row()) out.push_back(rowToAiAutoRule(s));
    return out;
}

// --- Betrodda avsändare av fjärrinnehåll ----------------------------------------

std::vector<std::string> Database::listRemoteSenders(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT sender FROM remote_senders WHERE user_id = ? ORDER BY sender");
    s.bind(1, userId);
    std::vector<std::string> out;
    while (s.row()) out.push_back(s.text(0));
    return out;
}

void Database::addRemoteSender(long long userId, const std::string& sender) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT IGNORE INTO remote_senders (user_id, sender_hash, sender, created_at) VALUES (?,?,?,UNIX_TIMESTAMP())");
    s.bind(1, userId).bind(2, crypto::toHex(crypto::sha256(sender))).bind(3, sender);
    if (!s.run()) throw std::runtime_error("Could not save the sender");
}

void Database::deleteRemoteSender(long long userId, const std::string& sender) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM remote_senders WHERE user_id = ? AND sender_hash = ?").bind(1, userId)
        .bind(2, crypto::toHex(crypto::sha256(sender))).run();
}

// --- Det AI-sorteringen har lärt sig ---------------------------------------------

namespace {

const char* kExampleColumns =
    "id, user_id, account_id, sender_email, sender_name, subject, source_folder, suggested, chosen, created_at";

AiSortExample rowToExample(Stmt& s) {
    AiSortExample e;
    e.id = s.int64(0);
    e.userId = s.int64(1);
    e.accountId = s.int64(2);
    e.senderEmail = s.text(3);
    e.senderName = s.text(4);
    e.subject = s.text(5);
    e.sourceFolder = s.text(6);
    e.suggested = s.text(7);
    e.chosen = s.text(8);
    e.createdAt = s.int64(9);
    return e;
}

std::string senderHash(const std::string& sender) {
    return crypto::toHex(crypto::sha256(sender));
}

// Gör en sträng ofarlig i LIKE, där %, _ och \ annars har särskild betydelse.
std::string escapeLike(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace

bool Database::addAiSortExample(const AiSortExample& e) {
    Lock lock(mutex_);
    {
        Stmt s(db_, "SELECT COUNT(*) FROM ai_sort_examples WHERE account_id = ? AND sender_hash = ? AND subject = ? AND "
                    "suggested = ? AND chosen = ?");
        s.bind(1, e.accountId).bind(2, senderHash(e.senderEmail)).bind(3, e.subject).bind(4, e.suggested).bind(5, e.chosen);
        if (s.row() && s.int64(0) > 0) return false;
    }
    Stmt s(db_, "INSERT INTO ai_sort_examples (user_id, account_id, sender_hash, sender_email, sender_name, subject, "
                "source_folder, suggested, chosen, corrected, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, e.userId).bind(2, e.accountId).bind(3, senderHash(e.senderEmail)).bind(4, e.senderEmail)
        .bind(5, e.senderName).bind(6, e.subject).bind(7, e.sourceFolder).bind(8, e.suggested).bind(9, e.chosen)
        .bind(10, e.suggested != e.chosen).bind(11, static_cast<long long>(e.createdAt));
    if (!s.run()) throw std::runtime_error("Could not save the correction");
    return true;
}

void Database::pruneAiSortExamples(long long accountId, int keep) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT id FROM ai_sort_examples WHERE account_id = ? ORDER BY id DESC LIMIT 1 OFFSET ?");
    s.bind(1, accountId).bind(2, keep);
    if (!s.row()) return;
    Stmt(db_, "DELETE FROM ai_sort_examples WHERE account_id = ? AND id <= ?").bind(1, accountId).bind(2, s.int64(0)).run();
}

std::vector<AiSortExample> Database::listAiSortCorrections(long long userId, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kExampleColumns +
                    " FROM ai_sort_examples WHERE user_id = ? AND corrected = 1 ORDER BY id DESC LIMIT ?");
    s.bind(1, userId).bind(2, limit);
    std::vector<AiSortExample> out;
    while (s.row()) out.push_back(rowToExample(s));
    return out;
}

std::vector<AiSortExample> Database::aiSortCorrectionsFor(long long accountId, const std::string& sender, int limit) {
    Lock lock(mutex_);
    std::string sql = std::string("SELECT ") + kExampleColumns + " FROM ai_sort_examples WHERE account_id = ? AND corrected = 1";
    if (!sender.empty()) sql += sender[0] == '@' ? " AND sender_email LIKE ?" : " AND sender_hash = ?";
    sql += " ORDER BY id DESC LIMIT ?";
    Stmt s(db_, sql);
    int i = 1;
    s.bind(i++, accountId);
    if (!sender.empty()) s.bind(i++, sender[0] == '@' ? "%" + escapeLike(sender) : senderHash(sender));
    s.bind(i, limit);
    std::vector<AiSortExample> out;
    while (s.row()) out.push_back(rowToExample(s));
    return out;
}

std::vector<AiSortExample> Database::aiSortDecisionsFor(long long accountId, const std::string& senderEmail, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kExampleColumns +
                    " FROM ai_sort_examples WHERE account_id = ? AND sender_hash = ? ORDER BY id DESC LIMIT ?");
    s.bind(1, accountId).bind(2, senderHash(senderEmail)).bind(3, limit);
    std::vector<AiSortExample> out;
    while (s.row()) out.push_back(rowToExample(s));
    return out;
}

std::vector<std::pair<long long, std::string>> Database::aiSortFrequentSenders(long long userId, int minCount, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT account_id, MIN(sender_email) FROM ai_sort_examples WHERE user_id = ? "
                "GROUP BY account_id, sender_hash HAVING COUNT(*) >= ? ORDER BY MAX(id) DESC LIMIT ?");
    s.bind(1, userId).bind(2, minCount).bind(3, limit);
    std::vector<std::pair<long long, std::string>> out;
    while (s.row()) out.emplace_back(s.int64(0), s.text(1));
    return out;
}

bool Database::deleteAiSortExample(long long id, long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, "DELETE FROM ai_sort_examples WHERE id = ? AND user_id = ?");
    s.bind(1, id).bind(2, userId);
    return s.run() && s.affectedRows() > 0;
}

void Database::clearAiSortExamples(long long userId) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM ai_sort_examples WHERE user_id = ?").bind(1, userId).run();
}

namespace {

MailRule rowToRule(Stmt& s) {
    MailRule r;
    r.id = s.int64(0);
    r.userId = s.int64(1);
    r.accountId = s.int64(2);
    r.name = s.text(3);
    r.enabled = s.int32(4) != 0;
    r.position = s.int32(5);
    r.matchAll = s.int32(6) != 0;
    r.conditions = s.text(7);
    r.actions = s.text(8);
    r.createdAt = s.int64(9);
    r.updatedAt = s.int64(10);
    return r;
}

RuleState rowToRuleState(Stmt& s) {
    RuleState r;
    r.accountId = s.int64(0);
    r.uidValidity = s.int64(1);
    r.lastUid = s.int64(2);
    r.nextCheckAt = s.int64(3);
    r.lastRunAt = s.int64(4);
    r.lastError = s.text(5);
    return r;
}

}  // namespace

std::vector<MailRule> Database::listRules(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRuleColumns + " FROM mail_rules WHERE user_id = ? ORDER BY account_id, position, id");
    s.bind(1, userId);
    std::vector<MailRule> out;
    while (s.row()) out.push_back(rowToRule(s));
    return out;
}

std::vector<MailRule> Database::rulesFor(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRuleColumns + " FROM mail_rules WHERE account_id = ? ORDER BY position, id");
    s.bind(1, accountId);
    std::vector<MailRule> out;
    while (s.row()) out.push_back(rowToRule(s));
    return out;
}

std::optional<MailRule> Database::getRule(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRuleColumns + " FROM mail_rules WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToRule(s);
    return std::nullopt;
}

long long Database::addRule(const MailRule& r) {
    Lock lock(mutex_);
    long long position = 0;
    {
        Stmt s(db_, "SELECT COALESCE(MAX(position), 0) + 1 FROM mail_rules WHERE account_id = ?");
        s.bind(1, r.accountId);
        s.row();
        position = s.int64(0);
    }
    Stmt s(db_, "INSERT INTO mail_rules (user_id, account_id, name, enabled, position, match_all, conditions, actions, "
                "created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, r.userId).bind(2, r.accountId).bind(3, r.name).bind(4, r.enabled).bind(5, position).bind(6, r.matchAll)
        .bind(7, r.conditions).bind(8, r.actions).bind(9, static_cast<long long>(r.createdAt))
        .bind(10, static_cast<long long>(r.updatedAt));
    if (!s.run()) throw std::runtime_error("Could not save the rule");
    return s.insertId();
}

void Database::updateRule(const MailRule& r) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE mail_rules SET name = ?, enabled = ?, match_all = ?, conditions = ?, actions = ?, updated_at = ? "
                "WHERE id = ? AND user_id = ?");
    s.bind(1, r.name).bind(2, r.enabled).bind(3, r.matchAll).bind(4, r.conditions).bind(5, r.actions)
        .bind(6, static_cast<long long>(r.updatedAt)).bind(7, r.id).bind(8, r.userId);
    if (!s.run()) throw std::runtime_error("Could not save the rule");
}

bool Database::deleteRule(long long id, long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, "DELETE FROM mail_rules WHERE id = ? AND user_id = ?");
    s.bind(1, id).bind(2, userId);
    return s.run() && s.affectedRows() > 0;
}

void Database::reorderRules(long long accountId, const std::vector<long long>& ids) {
    Lock lock(mutex_);
    int position = 1;
    for (long long id : ids) {
        Stmt s(db_, "UPDATE mail_rules SET position = ? WHERE id = ? AND account_id = ?");
        s.bind(1, position++).bind(2, id).bind(3, accountId).run();
    }
    // Regler som inte nämndes behåller sin inbördes ordning efter de nämnda.
    Stmt rest(db_, "SELECT id FROM mail_rules WHERE account_id = ? ORDER BY position, id");
    rest.bind(1, accountId);
    std::vector<long long> others;
    while (rest.row()) {
        long long id = rest.int64(0);
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) others.push_back(id);
    }
    for (long long id : others) {
        Stmt s(db_, "UPDATE mail_rules SET position = ? WHERE id = ?");
        s.bind(1, position++).bind(2, id).run();
    }
}

std::optional<RuleState> Database::getRuleState(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kRuleStateColumns + " FROM rule_state WHERE account_id = ?");
    s.bind(1, accountId);
    if (s.row()) return rowToRuleState(s);
    return std::nullopt;
}

void Database::saveRuleState(const RuleState& r) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO rule_state (account_id, uid_validity, last_uid, next_check_at, last_run_at, last_error) "
                "VALUES (?,?,?,?,?,?) ON DUPLICATE KEY UPDATE uid_validity = VALUES(uid_validity), last_uid = VALUES(last_uid), "
                "next_check_at = VALUES(next_check_at), last_run_at = VALUES(last_run_at), last_error = VALUES(last_error)");
    s.bind(1, r.accountId).bind(2, static_cast<long long>(r.uidValidity)).bind(3, static_cast<long long>(r.lastUid))
        .bind(4, static_cast<long long>(r.nextCheckAt)).bind(5, static_cast<long long>(r.lastRunAt)).bind(6, r.lastError);
    if (!s.run()) throw std::runtime_error("Could not save the rule state");
}

std::vector<RuleState> Database::dueRuleStates(std::int64_t now) {
    Lock lock(mutex_);
    // Konton med minst en påslagen regel. Ett konto utan sparat läge är förfallet: första kontrollen sätter märket.
    Stmt s(db_, "SELECT a.id, COALESCE(s.uid_validity, 0), COALESCE(s.last_uid, 0), COALESCE(s.next_check_at, 0), "
                "COALESCE(s.last_run_at, 0), COALESCE(s.last_error, '') FROM accounts a LEFT JOIN rule_state s ON s.account_id = a.id "
                "WHERE EXISTS (SELECT 1 FROM mail_rules r WHERE r.account_id = a.id AND r.enabled = 1) "
                "AND COALESCE(s.next_check_at, 0) <= ? ORDER BY a.id");
    s.bind(1, static_cast<long long>(now));
    std::vector<RuleState> out;
    while (s.row()) out.push_back(rowToRuleState(s));
    return out;
}

void Database::setRuleCheckDue(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE rule_state SET next_check_at = 0 WHERE account_id = ?");
    s.bind(1, accountId).run();
}

std::vector<DismissedSender> Database::listDismissedSenders(long long userId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT account_id, sender FROM rule_dismissed WHERE user_id = ?");
    s.bind(1, userId);
    std::vector<DismissedSender> out;
    while (s.row()) out.push_back({s.int64(0), s.text(1)});
    return out;
}

void Database::addDismissedSender(long long userId, long long accountId, const std::string& sender) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT IGNORE INTO rule_dismissed (user_id, account_id, sender_hash, sender, created_at) VALUES (?,?,?,?,?)");
    s.bind(1, userId).bind(2, accountId).bind(3, senderHash(sender)).bind(4, sender)
        .bind(5, static_cast<long long>(std::time(nullptr)));
    s.run();
}

void Database::renameAiLearningFolder(long long accountId, const std::string& from, const std::string& to) {
    Lock lock(mutex_);
    static const std::pair<const char*, const char*> columns[] = {{"ai_sort_examples", "source_folder"},
                                                                  {"ai_sort_examples", "suggested"},
                                                                  {"ai_sort_examples", "chosen"}};
    for (const auto& [table, column] : columns) {
        // Mappen själv och dess undermappar ("from/…" eller "from.…").
        std::string sql = std::string("UPDATE ") + table + " SET " + column + " = CONCAT(?, SUBSTRING(" + column +
                          ", ?)) WHERE account_id = ? AND (" + column + " = ? OR " + column + " LIKE ? OR " + column +
                          " LIKE ?)";
        long long chars = 0;  // SUBSTRING räknar tecken, inte byte.
        for (unsigned char c : from) chars += (c & 0xC0) != 0x80;
        Stmt s(db_, sql);
        s.bind(1, to).bind(2, chars + 1).bind(3, accountId).bind(4, from)
            .bind(5, escapeLike(from) + "/%").bind(6, escapeLike(from) + ".%");
        s.run();
    }
}

AiSortJob Database::rowToAiSortJob(Stmt& s) {
    AiSortJob j;
    j.id = s.int64(0);
    j.userId = s.int64(1);
    j.accountId = s.int64(2);
    j.folder = s.text(3);
    j.targets = s.text(4);
    j.unreadOnly = s.int32(5) != 0;
    j.state = s.text(6);
    j.beforeUid = s.int64(7);
    j.total = s.int64(8);
    j.processed = s.int64(9);
    j.moved = s.int64(10);
    j.undoCursor = s.int64(11);
    j.failures = s.int32(12);
    j.lastError = s.text(13);
    j.nextRunAt = s.int64(14);
    j.createdAt = s.int64(15);
    j.finishedAt = s.int64(16);
    return j;
}

long long Database::addAiSortJob(const AiSortJob& j) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO ai_sort_jobs (user_id, account_id, folder, targets, unread_only, state, before_uid, total, "
                "created_at) VALUES (?,?,?,?,?,?,?,?,?)");
    s.bind(1, j.userId).bind(2, j.accountId).bind(3, j.folder).bind(4, j.targets).bind(5, j.unreadOnly).bind(6, j.state)
        .bind(7, static_cast<long long>(j.beforeUid)).bind(8, static_cast<long long>(j.total))
        .bind(9, static_cast<long long>(j.createdAt));
    if (!s.run()) throw std::runtime_error("Could not start the sorting");
    return s.insertId();
}

std::optional<AiSortJob> Database::getAiSortJob(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kSortJobColumns + " FROM ai_sort_jobs WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToAiSortJob(s);
    return std::nullopt;
}

std::vector<AiSortJob> Database::listAiSortJobs(long long userId, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kSortJobColumns + " FROM ai_sort_jobs WHERE user_id = ? ORDER BY id DESC LIMIT ?");
    s.bind(1, userId).bind(2, limit);
    std::vector<AiSortJob> out;
    while (s.row()) out.push_back(rowToAiSortJob(s));
    return out;
}

std::vector<AiSortJob> Database::dueAiSortJobs(std::int64_t now) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kSortJobColumns +
                    " FROM ai_sort_jobs WHERE state IN ('running', 'undoing') AND next_run_at <= ? ORDER BY next_run_at, id");
    s.bind(1, static_cast<long long>(now));
    std::vector<AiSortJob> out;
    while (s.row()) out.push_back(rowToAiSortJob(s));
    return out;
}

bool Database::hasActiveAiSortJob(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM ai_sort_jobs WHERE account_id = ? AND state IN ('running', 'paused', 'undoing')");
    s.bind(1, accountId);
    return s.row() && s.int64(0) > 0;
}

void Database::setAiSortJobProgress(AiSortJob& j) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE ai_sort_jobs SET before_uid = ?, processed = ?, moved = GREATEST(moved + ?, 0), undo_cursor = ?, "
                "failures = ?, last_error = ?, next_run_at = ? WHERE id = ?");
    s.bind(1, static_cast<long long>(j.beforeUid)).bind(2, static_cast<long long>(j.processed))
        .bind(3, static_cast<long long>(j.movedDelta)).bind(4, static_cast<long long>(j.undoCursor)).bind(5, j.failures)
        .bind(6, j.lastError).bind(7, static_cast<long long>(j.nextRunAt)).bind(8, j.id).run();
    j.movedDelta = 0;
}

void Database::addAiSortJobMoved(long long id, std::int64_t delta) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE ai_sort_jobs SET moved = GREATEST(moved + ?, 0) WHERE id = ?")
        .bind(1, static_cast<long long>(delta)).bind(2, id).run();
}

bool Database::setAiSortJobState(long long id, const std::vector<std::string>& from, const std::string& to,
                                 std::int64_t now) {
    Lock lock(mutex_);
    std::string in;
    for (std::size_t i = 0; i < from.size(); ++i) in += i ? ",?" : "?";
    bool finished = to == "done" || to == "cancelled" || to == "undone";
    Stmt s(db_, "UPDATE ai_sort_jobs SET state = ?, finished_at = ?, next_run_at = 0, failures = 0 WHERE id = ? AND state IN (" + in + ")");
    s.bind(1, to).bind(2, finished ? static_cast<long long>(now) : 0LL).bind(3, id);
    for (std::size_t i = 0; i < from.size(); ++i) s.bind(static_cast<int>(4 + i), from[i]);
    return s.run() && s.affectedRows() > 0;
}

void Database::renameAiSortJobFolder(long long accountId, const std::string& from, const std::string& to) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE ai_sort_jobs SET folder = ? WHERE account_id = ? AND folder = ?").bind(1, to).bind(2, accountId)
        .bind(3, from).run();
}

std::map<std::string, std::int64_t> Database::aiSortJobTargets(long long jobId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT target_folder, COUNT(*) FROM ai_sort_log WHERE job_id = ? AND undone_at = 0 GROUP BY target_folder");
    s.bind(1, jobId);
    std::map<std::string, std::int64_t> out;
    while (s.row()) out[s.text(0)] = s.int64(1);
    return out;
}

std::vector<AiSortLogEntry> Database::aiSortJobMoves(long long jobId, long long beforeId, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kSortLogColumns +
                    " FROM ai_sort_log WHERE job_id = ? AND undone_at = 0 AND (? = 0 OR id < ?) ORDER BY id DESC LIMIT ?");
    s.bind(1, jobId).bind(2, beforeId).bind(3, beforeId).bind(4, limit);
    std::vector<AiSortLogEntry> out;
    while (s.row()) out.push_back(rowToAiSortLogEntry(s));
    return out;
}

AiSortLogEntry Database::rowToAiSortLogEntry(Stmt& s) {
    AiSortLogEntry e;
    e.id = s.int64(0);
    e.userId = s.int64(1);
    e.accountId = s.int64(2);
    e.messageId = s.text(3);
    e.subject = s.text(4);
    e.sender = s.text(5);
    e.sourceFolder = s.text(6);
    e.targetFolder = s.text(7);
    e.reason = s.text(8);
    e.movedAt = s.int64(9);
    e.undoneAt = s.int64(10);
    e.jobId = s.int64(11);
    e.ruleId = s.int64(12);
    return e;
}

long long Database::addAiSortLog(const AiSortLogEntry& e) {
    Lock lock(mutex_);
    Stmt s(db_, "INSERT INTO ai_sort_log (user_id, account_id, message_hash, message_id, subject, sender, "
                "source_folder, target_folder, reason, moved_at, job_id, rule_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, e.userId).bind(2, e.accountId).bind(3, crypto::toHex(crypto::sha256(e.messageId))).bind(4, e.messageId)
        .bind(5, e.subject).bind(6, e.sender).bind(7, e.sourceFolder).bind(8, e.targetFolder).bind(9, e.reason)
        .bind(10, static_cast<long long>(e.movedAt)).bind(11, e.jobId).bind(12, e.ruleId);
    if (!s.run()) throw std::runtime_error("Could not save the sorting history");
    return s.insertId();
}

std::optional<AiSortLogEntry> Database::getAiSortLog(long long id) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kSortLogColumns + " FROM ai_sort_log WHERE id = ?");
    s.bind(1, id);
    if (s.row()) return rowToAiSortLogEntry(s);
    return std::nullopt;
}

std::vector<AiSortLogEntry> Database::listAiSortLog(long long userId, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kSortLogColumns + " FROM ai_sort_log WHERE user_id = ? ORDER BY id DESC LIMIT ?");
    s.bind(1, userId).bind(2, limit);
    std::vector<AiSortLogEntry> out;
    while (s.row()) out.push_back(rowToAiSortLogEntry(s));
    return out;
}

void Database::markAiSortLogUndone(long long id, std::int64_t now) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE ai_sort_log SET undone_at = ? WHERE id = ?").bind(1, static_cast<long long>(now)).bind(2, id).run();
}

bool Database::wasAiSorted(long long accountId, const std::string& messageId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*) FROM ai_sort_log WHERE account_id = ? AND message_hash = ?");
    s.bind(1, accountId).bind(2, crypto::toHex(crypto::sha256(messageId)));
    s.row();
    return s.int32(0) > 0;
}

void Database::deleteAiSortLog(long long id) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM ai_sort_log WHERE id = ?").bind(1, id).run();
}

void Database::pruneAiSortLog(std::int64_t olderThan) {
    Lock lock(mutex_);
    Stmt(db_, "DELETE FROM ai_sort_log WHERE moved_at < ?").bind(1, static_cast<long long>(olderThan)).run();
}

}  // namespace brevduva
