// Databasdelen av mejlcachen: mappar, rubriker och meddelandetext.
#include "db.hpp"

#include "crypto.hpp"
#include "db_stmt.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <variant>

namespace brevduva {

namespace {

using Lock = std::lock_guard<std::recursive_mutex>;

const char* kFolderColumns =
    "id, account_id, path, delim, special_use, selectable, sort_order, total, unread, uid_validity, uid_next, "
    "synced_uid_validity, synced_uid_next, synced_total, synced_unread, headers_complete, dirty_at, reconciled_at, "
    "reset_modseq";

const char* kMessageColumns =
    "id, uid, subject, sender, recipients, date, size, seen, flagged, answered, draft, has_attachments, body_state, modseq";


// ORDER BY för en lista eller sökning. Ämnet sorteras utan svarsprefix (Re:, Sv:, Fwd: …) och avsändare och mottagare
// på namnet, eller adressen när namn saknas, alltså som de visas i listan, i svensk bokstavsordning (å, ä och ö sist).
// Vid lika värden kommer nyaste först, och vid lika datum det som kom sist till mappen (högst UID). Datumordningen i en
// mapp täcks av indexet cache_messages_date.
std::string orderBy(const std::string& sort, bool descending) {
    std::string dir = descending ? " DESC" : " ASC";
    std::string tiebreak = ", m.date DESC, m.uid DESC";
    const std::string collate = " COLLATE utf8mb4_swedish_ci";
    if (sort == "date") return " ORDER BY m.date" + dir + ", m.uid" + dir;
    if (sort == "size") return " ORDER BY m.size" + dir + tiebreak;
    if (sort == "subject") {
        return " ORDER BY REGEXP_REPLACE(m.subject, '(?i)^([[:space:]]*(re|sv|fwd|fw|vb|aw|wg|tr)[[:space:]]*(\\\\[[0-9]+\\\\])?[[:space:]]*:)+[[:space:]]*', '')" +
               collate + dir + tiebreak;
    }
    if (sort == "from" || sort == "to") {
        std::string column = sort == "from" ? "m.sender" : "m.recipients";
        return " ORDER BY COALESCE(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(" + column + ", '$[0].name')), ''), "
               "JSON_UNQUOTE(JSON_EXTRACT(" + column + ", '$[0].email')), '')" + collate + dir + tiebreak;
    }
    return " ORDER BY m.uid" + dir;
}

std::string pathHash(const std::string& path) {
    return crypto::toHex(crypto::sha256(path));
}

// Komprimerad text med längden först, så att uppackningen vet hur stor bufferten ska vara.
std::string deflate(const std::string& data) {
    if (data.empty()) return {};
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::string out(size + 8, '\0');
    std::uint64_t length = data.size();
    for (int i = 0; i < 8; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>((length >> (8 * i)) & 0xff);
    if (compress2(reinterpret_cast<Bytef*>(&out[8]), &size, reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("Kunde inte komprimera meddelandetexten");
    }
    out.resize(size + 8);
    return out;
}

std::string inflate(const std::string& data) {
    if (data.size() < 8) return {};
    std::uint64_t length = 0;
    for (int i = 0; i < 8; ++i) length |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[static_cast<std::size_t>(i)])) << (8 * i);
    if (length > 64ull * 1024 * 1024) throw std::runtime_error("Skadad text i mejlcachen");
    std::string out(length, '\0');
    uLongf size = static_cast<uLongf>(length);
    if (uncompress(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(data.data() + 8),
                   static_cast<uLong>(data.size() - 8)) != Z_OK) {
        throw std::runtime_error("Skadad text i mejlcachen");
    }
    out.resize(size);
    return out;
}

std::string uidList(const std::vector<std::uint32_t>& uids) {
    std::string out;
    for (auto uid : uids) out += (out.empty() ? "" : ",") + std::to_string(uid);
    return out;
}

bool isChildPath(const std::string& path, const std::string& parent, const std::string& delimiter) {
    return !delimiter.empty() && path.size() > parent.size() + delimiter.size() - 1 &&
           path.compare(0, parent.size(), parent) == 0 && path.compare(parent.size(), delimiter.size(), delimiter) == 0;
}

// Sökord för fulltextsökning i boolean mode: varje ord måste finnas, med prefixmatchning. Operatortecken tas bort.
// Ord kortare än tre tecken indexeras inte av MariaDB och hoppas över.
std::string fulltextQuery(const std::string& text) {
    std::string out;
    std::string word;
    auto flush = [&]() {
        std::size_t chars = 0;
        for (unsigned char c : word) chars += (c & 0xC0) != 0x80;
        if (chars >= 3) out += (out.empty() ? "+" : " +") + word + "*";
        word.clear();
    };
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) word += static_cast<char>(c);
        else flush();
    }
    flush();
    return out;
}

std::string likePattern(const std::string& text) {
    std::string out = "%";
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    return out + "%";
}

CacheFolder rowToFolder(Stmt& s) {
    CacheFolder f;
    f.id = s.int64(0);
    f.accountId = s.int64(1);
    f.path = s.text(2);
    f.delimiter = s.text(3);
    f.specialUse = s.text(4);
    f.selectable = s.int32(5) != 0;
    f.position = s.int32(6);
    f.total = s.int64(7);
    f.unread = s.int64(8);
    f.uidValidity = s.int64(9);
    f.uidNext = s.int64(10);
    f.syncedUidValidity = s.int64(11);
    f.syncedUidNext = s.int64(12);
    f.syncedTotal = s.int64(13);
    f.syncedUnread = s.int64(14);
    f.headersComplete = s.int32(15) != 0;
    f.dirtyAt = s.int64(16);
    f.reconciledAt = s.int64(17);
    f.resetModseq = s.int64(18);
    return f;
}

CacheMessage rowToMessage(Stmt& s) {
    CacheMessage m;
    m.id = s.int64(0);
    m.uid = static_cast<std::uint32_t>(s.int64(1));
    m.subject = s.text(2);
    m.sender = s.text(3);
    m.recipients = s.text(4);
    m.date = s.int64(5);
    m.size = s.int64(6);
    m.flags.seen = s.int32(7) != 0;
    m.flags.flagged = s.int32(8) != 0;
    m.flags.answered = s.int32(9) != 0;
    m.flags.draft = s.int32(10) != 0;
    m.hasAttachments = s.int32(11) != 0;
    m.bodyState = s.int32(12);
    m.modseq = s.int64(13);
    return m;
}

}  // namespace

void Database::begin() {
    exec("START TRANSACTION");
    inTx_ = true;
}

void Database::commit() {
    exec("COMMIT");
    inTx_ = false;
}

void Database::rollback() {
    exec("ROLLBACK");
    inTx_ = false;
}

void Database::writeTx(const std::function<void()>& f) {
    Lock lock(mutex_);
    if (inTx_) {
        f();
        return;
    }
    begin();
    try {
        f();
        commit();
    } catch (...) {
        rollback();
        throw;
    }
}

std::int64_t Database::allocModseq() {
    Lock lock(mutex_);
    // Radlåset på cache_seq hålls tills transaktionen är klar, så numren blir synliga i nummerordning.
    Stmt u(db_, "UPDATE cache_seq SET value = LAST_INSERT_ID(value + 1) WHERE id = 1");
    u.run();
    if (u.affectedRows() == 0) {
        exec("INSERT IGNORE INTO cache_seq (id, value) VALUES (1, 0)");
        Stmt(db_, "UPDATE cache_seq SET value = LAST_INSERT_ID(value + 1) WHERE id = 1").run();
    }
    Stmt s(db_, "SELECT LAST_INSERT_ID()");
    s.row();
    return s.int64(0);
}

CacheSeq Database::cacheSeq() {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT value, pruned_modseq FROM cache_seq WHERE id = 1");
    CacheSeq seq;
    if (s.row()) {
        seq.value = s.int64(0);
        seq.pruned = s.int64(1);
    }
    return seq;
}

// --- Mappar ---------------------------------------------------------------------

std::vector<CacheFolder> Database::listCacheFolders(long long accountId) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kFolderColumns + " FROM cache_folders WHERE account_id = ? ORDER BY sort_order, id");
    s.bind(1, accountId);
    std::vector<CacheFolder> out;
    while (s.row()) out.push_back(rowToFolder(s));
    return out;
}

std::optional<CacheFolder> Database::getCacheFolder(long long accountId, const std::string& path) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kFolderColumns + " FROM cache_folders WHERE account_id = ? AND path_hash = ?");
    s.bind(1, accountId).bind(2, pathHash(path));
    if (s.row()) return rowToFolder(s);
    return std::nullopt;
}

void Database::saveCacheFolderList(long long accountId, const std::vector<CacheFolder>& folders) {
    Lock lock(mutex_);
    std::string keep;
    for (const auto& f : folders) {
        std::string hash = pathHash(f.path);
        Stmt s(db_, "INSERT INTO cache_folders (account_id, path_hash, path, delim, special_use, selectable, sort_order, "
                    "total, unread, uid_validity, uid_next) VALUES (?,?,?,?,?,?,?,?,?,?,?) "
                    "ON DUPLICATE KEY UPDATE delim = VALUES(delim), special_use = VALUES(special_use), "
                    "selectable = VALUES(selectable), sort_order = VALUES(sort_order), total = VALUES(total), "
                    "unread = VALUES(unread), uid_validity = VALUES(uid_validity), uid_next = VALUES(uid_next)");
        s.bind(1, accountId).bind(2, hash).bind(3, f.path).bind(4, f.delimiter).bind(5, f.specialUse).bind(6, f.selectable)
            .bind(7, f.position).bind(8, static_cast<long long>(f.total)).bind(9, static_cast<long long>(f.unread))
            .bind(10, static_cast<long long>(f.uidValidity)).bind(11, static_cast<long long>(f.uidNext));
        s.run();
        keep += (keep.empty() ? "'" : ",'") + hash + "'";  // Hexsträngar, säkra att sätta in i satsen.
    }
    std::string sql = "DELETE FROM cache_folders WHERE account_id = ?";
    if (!keep.empty()) sql += " AND path_hash NOT IN (" + keep + ")";
    Stmt(db_, sql).bind(1, accountId).run();
}

void Database::saveCacheFolderSync(const CacheFolder& f, std::int64_t startedAt) {
    Lock lock(mutex_);
    Stmt s(db_, "UPDATE cache_folders SET synced_uid_validity = ?, synced_uid_next = ?, synced_total = ?, "
                "synced_unread = ?, headers_complete = ?, reconciled_at = ?, "
                "dirty_at = IF(dirty_at < ?, 0, dirty_at) WHERE id = ?");
    s.bind(1, static_cast<long long>(f.syncedUidValidity)).bind(2, static_cast<long long>(f.syncedUidNext))
        .bind(3, static_cast<long long>(f.syncedTotal)).bind(4, static_cast<long long>(f.syncedUnread))
        .bind(5, f.headersComplete).bind(6, static_cast<long long>(f.reconciledAt))
        .bind(7, static_cast<long long>(startedAt)).bind(8, f.id).run();
}

void Database::markCacheFolderDirty(long long accountId, const std::string& path, std::int64_t now) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE cache_folders SET dirty_at = ? WHERE account_id = ? AND path_hash = ?")
        .bind(1, static_cast<long long>(now)).bind(2, accountId).bind(3, pathHash(path)).run();
}

void Database::adjustCacheFolderCounts(long long folderId, std::int64_t total, std::int64_t unread, bool alsoSynced) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE cache_folders SET total = GREATEST(0, total + ?), unread = GREATEST(0, unread + ?) WHERE id = ?")
        .bind(1, static_cast<long long>(total)).bind(2, static_cast<long long>(unread)).bind(3, folderId).run();
    if (!alsoSynced) return;
    Stmt(db_, "UPDATE cache_folders SET synced_total = GREATEST(0, synced_total + ?), "
              "synced_unread = GREATEST(0, synced_unread + ?) WHERE id = ? AND synced_total >= 0")
        .bind(1, static_cast<long long>(total)).bind(2, static_cast<long long>(unread)).bind(3, folderId).run();
}

Database::CacheCounts Database::cacheCounts(long long folderId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT COUNT(*), COALESCE(SUM(seen = 0), 0), COALESCE(MAX(uid), 0) FROM cache_messages WHERE folder_id = ?");
    s.bind(1, folderId);
    s.row();
    CacheCounts c;
    c.count = s.int64(0);
    c.unread = s.int64(1);
    c.maxUid = s.int64(2);
    return c;
}

void Database::renameCacheFolder(long long accountId, const std::string& from, const std::string& to) {
    Lock lock(mutex_);
    for (const auto& f : listCacheFolders(accountId)) {
        if (f.path != from && !isChildPath(f.path, from, f.delimiter)) continue;
        std::string path = to + f.path.substr(from.size());
        // En gammal rad med målets sökväg (t.ex. efter en tidigare flytt) skulle krocka med den unika nyckeln.
        Stmt(db_, "DELETE FROM cache_folders WHERE account_id = ? AND path_hash = ? AND id != ?")
            .bind(1, accountId).bind(2, pathHash(path)).bind(3, f.id).run();
        Stmt(db_, "UPDATE cache_folders SET path = ?, path_hash = ? WHERE id = ?")
            .bind(1, path).bind(2, pathHash(path)).bind(3, f.id).run();
    }
}

void Database::deleteCacheFolder(long long accountId, const std::string& path) {
    Lock lock(mutex_);
    for (const auto& f : listCacheFolders(accountId)) {
        if (f.path != path && !isChildPath(f.path, path, f.delimiter)) continue;
        Stmt(db_, "DELETE FROM cache_folders WHERE id = ?").bind(1, f.id).run();
    }
}

void Database::purgeCacheMessages(long long folderId) {
    Lock lock(mutex_);
    writeTx([&] {
        // Mappen börjar om: gamla gravstenar behövs inte, och klienter får veta via reset_modseq att de ska hämta om den.
        std::int64_t modseq = allocModseq();
        Stmt(db_, "DELETE FROM cache_tombstones WHERE folder_id = ?").bind(1, folderId).run();
        Stmt(db_, "DELETE FROM cache_messages WHERE folder_id = ?").bind(1, folderId).run();
        Stmt(db_, "UPDATE cache_folders SET headers_complete = 0, synced_total = -1, synced_unread = -1, synced_uid_next = 0, "
                  "reset_modseq = ? WHERE id = ?").bind(1, static_cast<long long>(modseq)).bind(2, folderId).run();
    });
}

// --- Meddelanden ------------------------------------------------------------------

std::map<std::uint32_t, CacheFlags> Database::cacheMessageFlags(long long folderId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT uid, seen, flagged, answered, draft FROM cache_messages WHERE folder_id = ?");
    s.bind(1, folderId);
    std::map<std::uint32_t, CacheFlags> out;
    while (s.row()) {
        CacheFlags f;
        f.seen = s.int32(1) != 0;
        f.flagged = s.int32(2) != 0;
        f.answered = s.int32(3) != 0;
        f.draft = s.int32(4) != 0;
        out[static_cast<std::uint32_t>(s.int64(0))] = f;
    }
    return out;
}

void Database::upsertCacheMessage(long long folderId, const CacheMessage& m) {
    upsertCacheMessages(folderId, {m});
}

void Database::upsertCacheMessages(long long folderId, const std::vector<CacheMessage>& messages) {
    if (messages.empty()) return;
    Lock lock(mutex_);
    writeTx([&] {
        long long modseq = allocModseq();
        for (const auto& m : messages) {
            Stmt s(db_, "INSERT INTO cache_messages (folder_id, uid, subject, sender, recipients, date, size, seen, flagged, "
                        "answered, draft, has_attachments, modseq) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?) "
                        "ON DUPLICATE KEY UPDATE subject = VALUES(subject), sender = VALUES(sender), recipients = VALUES(recipients), "
                        "date = VALUES(date), size = VALUES(size), seen = VALUES(seen), flagged = VALUES(flagged), "
                        "answered = VALUES(answered), draft = VALUES(draft), has_attachments = VALUES(has_attachments), "
                        "modseq = VALUES(modseq)");
            s.bind(1, folderId).bind(2, static_cast<long long>(m.uid)).bind(3, m.subject).bind(4, m.sender).bind(5, m.recipients)
                .bind(6, static_cast<long long>(m.date)).bind(7, static_cast<long long>(m.size)).bind(8, m.flags.seen)
                .bind(9, m.flags.flagged).bind(10, m.flags.answered).bind(11, m.flags.draft).bind(12, m.hasAttachments)
                .bind(13, modseq);
            s.run();
        }
    });
}

void Database::setCacheMessageFlags(long long folderId, std::uint32_t uid, const CacheFlags& f) {
    Lock lock(mutex_);
    writeTx([&] {
        Stmt(db_, "UPDATE cache_messages SET seen = ?, flagged = ?, answered = ?, draft = ?, modseq = ? "
                  "WHERE folder_id = ? AND uid = ?")
            .bind(1, f.seen).bind(2, f.flagged).bind(3, f.answered).bind(4, f.draft).bind(5, static_cast<long long>(allocModseq()))
            .bind(6, folderId).bind(7, static_cast<long long>(uid)).run();
    });
}

std::int64_t Database::changeCacheFlags(long long folderId, const std::vector<std::uint32_t>& uids, const CacheFlags& add,
                                        const CacheFlags& remove) {
    if (uids.empty()) return 0;
    Lock lock(mutex_);
    std::string in = uidList(uids);
    std::int64_t unreadDelta = 0;
    writeTx([&] {
        long long modseq = allocModseq();
        auto apply = [&](const char* column, bool value) {
            Stmt s(db_, std::string("UPDATE cache_messages SET ") + column + " = ?, modseq = ? WHERE folder_id = ? AND " +
                            column + " != ? AND uid IN (" + in + ")");
            s.bind(1, value).bind(2, modseq).bind(3, folderId).bind(4, value).run();
            return s.affectedRows();
        };
        if (add.seen) unreadDelta -= apply("seen", true);
        if (remove.seen) unreadDelta += apply("seen", false);
        if (add.flagged) apply("flagged", true);
        if (remove.flagged) apply("flagged", false);
        if (add.answered) apply("answered", true);
        if (remove.answered) apply("answered", false);
        if (add.draft) apply("draft", true);
        if (remove.draft) apply("draft", false);
    });
    return unreadDelta;
}

CacheRemoved Database::removeCacheMessages(long long folderId, const std::vector<std::uint32_t>& uids) {
    CacheRemoved out;
    if (uids.empty()) return out;
    Lock lock(mutex_);
    std::string in = uidList(uids);
    writeTx([&] {
        {
            Stmt s(db_, "SELECT COUNT(*), COALESCE(SUM(seen = 0), 0) FROM cache_messages WHERE folder_id = ? AND uid IN (" + in + ")");
            s.bind(1, folderId);
            s.row();
            out.count = s.int64(0);
            out.unread = s.int64(1);
        }
        if (out.count == 0) return;
        // Gravstenar för det som faktiskt fanns, så att klienter som följer ändringarna tar bort samma rader.
        Stmt(db_, "INSERT INTO cache_tombstones (folder_id, uid, modseq) SELECT folder_id, uid, ? FROM cache_messages "
                  "WHERE folder_id = ? AND uid IN (" + in + ")")
            .bind(1, static_cast<long long>(allocModseq())).bind(2, folderId).run();
        Stmt(db_, "DELETE FROM cache_messages WHERE folder_id = ? AND uid IN (" + in + ")").bind(1, folderId).run();
    });
    return out;
}

std::vector<CacheMessage> Database::cacheMessagesWithoutBody(long long folderId, int limit) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kMessageColumns +
                    " FROM cache_messages WHERE folder_id = ? AND body_state = 0 ORDER BY uid DESC LIMIT ?");
    s.bind(1, folderId).bind(2, limit);
    std::vector<CacheMessage> out;
    while (s.row()) out.push_back(rowToMessage(s));
    return out;
}

void Database::saveCacheBody(long long messageRowId, int state, const CacheBody& body, const std::string& searchText) {
    Lock lock(mutex_);
    // Ta bort och lägg till i stället för ON DUPLICATE KEY UPDATE, som MariaDB 10.3 ibland svarar
    // "Can't find record" på i tabeller med fulltextindex.
    Stmt(db_, "DELETE FROM cache_bodies WHERE message_id = ?").bind(1, messageRowId).run();
    Stmt s(db_, "INSERT INTO cache_bodies (message_id, meta, text_z, html_z, search_text) VALUES (?,?,?,?,?)");
    s.bind(1, messageRowId).bind(2, body.meta).bindBlob(3, deflate(body.text)).bindBlob(4, deflate(body.html))
        .bind(5, searchText);
    s.run();
    setCacheBodyState(messageRowId, state);
}

void Database::setCacheBodyState(long long messageRowId, int state) {
    Lock lock(mutex_);
    Stmt(db_, "UPDATE cache_messages SET body_state = ? WHERE id = ?").bind(1, state).bind(2, messageRowId).run();
}

CacheListResult Database::listCacheMessages(long long folderId, const CacheListQuery& q) {
    Lock lock(mutex_);
    std::string where = " FROM cache_messages m WHERE m.folder_id = ?";
    if (q.unreadOnly) where += " AND m.seen = 0";
    if (q.flaggedOnly) where += " AND m.flagged = 1";
    if (q.beforeUid) where += " AND m.uid < " + std::to_string(q.beforeUid);
    std::string fulltext = fulltextQuery(q.text);
    if (!q.text.empty()) {
        where += " AND (m.subject LIKE ? OR m.sender LIKE ? OR m.recipients LIKE ?";
        if (!fulltext.empty()) {
            where += " OR m.id IN (SELECT b.message_id FROM cache_bodies b WHERE MATCH(b.search_text) AGAINST (? IN BOOLEAN MODE))";
        }
        where += ")";
    }
    auto bindWhere = [&](Stmt& s) {
        int i = 1;
        s.bind(i++, folderId);
        if (!q.text.empty()) {
            std::string like = likePattern(q.text);
            for (int k = 0; k < 3; ++k) s.bind(i++, like);
            if (!fulltext.empty()) s.bind(i++, fulltext);
        }
        return i;
    };

    CacheListResult out;
    {
        Stmt s(db_, "SELECT COUNT(*)" + where);
        bindWhere(s);
        s.row();
        out.total = s.int64(0);
    }
    Stmt s(db_, std::string("SELECT ") + kMessageColumns + where + orderBy(q.sort, q.descending) + " LIMIT ? OFFSET ?");
    int i = bindWhere(s);
    s.bind(i, q.pageSize).bind(i + 1, static_cast<long long>(q.page) * q.pageSize);
    while (s.row()) out.messages.push_back(rowToMessage(s));
    return out;
}

namespace {

// Ett alternativ i fulltextsökningen: ett ord blir "ord*", flera ord ("e-post") blir "(+ord1* +ord2*)".
std::string fulltextAlternative(const std::string& text) {
    std::string words = fulltextQuery(text);
    if (words.empty() || words.find(' ') == std::string::npos) return words.empty() ? words : words.substr(1);
    return "(" + words + ")";
}

}  // namespace

CacheSearchResult Database::searchCacheMessages(const CacheSearchQuery& q) {
    CacheSearchResult out;
    if (q.accountIds.empty()) return out;
    Lock lock(mutex_);

    std::vector<std::variant<long long, std::string>> binds;
    std::string where = " FROM cache_messages m JOIN cache_folders f ON f.id = m.folder_id WHERE f.account_id IN (";
    for (std::size_t i = 0; i < q.accountIds.size(); ++i) {
        where += i ? ",?" : "?";
        binds.emplace_back(q.accountIds[i]);
    }
    where += ")";
    // Virtuella mappar cachas inte; det som kan ligga kvar från förr ska inte ge dubbletter.
    where += " AND f.special_use NOT IN ('all', 'flagged', 'important')";
    if (q.skipSent) where += " AND f.special_use NOT IN ('sent', 'drafts')";
    if (!q.folder.empty()) {
        where += " AND f.path_hash = ?";
        binds.emplace_back(pathHash(q.folder));
    } else if (!q.includeTrash) {
        where += " AND f.special_use NOT IN ('trash', 'junk')";
    }
    for (const auto& group : q.terms) {
        std::string clause;
        std::string fulltext;
        for (const auto& alternative : group) {
            if (alternative.empty()) continue;
            clause += clause.empty() ? "" : " OR ";
            clause += "m.subject LIKE ? OR m.sender LIKE ? OR m.recipients LIKE ?";
            for (int k = 0; k < 3; ++k) binds.emplace_back(likePattern(alternative));
            std::string words = fulltextAlternative(alternative);
            if (!words.empty()) fulltext += (fulltext.empty() ? "" : " ") + words;
        }
        if (clause.empty()) continue;
        if (!fulltext.empty()) {
            clause += " OR m.id IN (SELECT b.message_id FROM cache_bodies b WHERE MATCH(b.search_text) AGAINST (? IN BOOLEAN MODE))";
            binds.emplace_back(fulltext);
        }
        where += " AND (" + clause + ")";
    }
    if (!q.from.empty()) {
        where += " AND m.sender LIKE ?";
        binds.emplace_back(likePattern(q.from));
    }
    if (!q.to.empty()) {
        where += " AND m.recipients LIKE ?";
        binds.emplace_back(likePattern(q.to));
    }
    if (q.after > 0) {
        where += " AND m.date >= ?";
        binds.emplace_back(static_cast<long long>(q.after));
    }
    if (q.before > 0) {
        where += " AND m.date < ?";
        binds.emplace_back(static_cast<long long>(q.before));
    }
    if (q.unreadOnly) where += " AND m.seen = 0";
    if (q.flaggedOnly) where += " AND m.flagged = 1";
    if (q.attachmentsOnly) where += " AND m.has_attachments = 1";

    auto bindWhere = [&](Stmt& s) {
        int i = 1;
        for (const auto& b : binds) {
            if (auto* n = std::get_if<long long>(&b)) s.bind(i++, *n);
            else s.bind(i++, std::get<std::string>(b));
        }
        return i;
    };
    {
        Stmt s(db_, "SELECT COUNT(*)" + where);
        bindWhere(s);
        s.row();
        out.total = s.int64(0);
    }
    Stmt s(db_, "SELECT m.id, m.uid, m.subject, m.sender, m.recipients, m.date, m.size, m.seen, m.flagged, m.answered, "
                "m.draft, m.has_attachments, m.body_state, m.modseq, f.account_id, f.path" +
                    where + orderBy(q.sort, q.descending) + " LIMIT ? OFFSET ?");
    int i = bindWhere(s);
    s.bind(i, q.pageSize).bind(i + 1, static_cast<long long>(q.page) * q.pageSize);
    while (s.row()) {
        CacheSearchHit hit;
        hit.message = rowToMessage(s);
        hit.accountId = s.int64(14);
        hit.folder = s.text(15);
        out.hits.push_back(std::move(hit));
    }
    return out;
}

// --- Ändringar för klienter med egen kopia -------------------------------------------

CacheChanges Database::listCacheChanges(long long accountId, std::int64_t since, std::int64_t cutoff, int limit) {
    Lock lock(mutex_);
    CacheChanges out;
    out.cutoff = cutoff;
    const std::string select =
        "SELECT m.id, m.uid, m.subject, m.sender, m.recipients, m.date, m.size, m.seen, m.flagged, m.answered, "
        "m.draft, m.has_attachments, m.body_state, m.modseq, f.path FROM cache_messages m "
        "JOIN cache_folders f ON f.id = m.folder_id WHERE f.account_id = ? AND m.modseq > ? AND m.modseq <= ?";
    auto readRows = [&](Stmt& s) {
        while (s.row()) {
            CacheChange c;
            c.message = rowToMessage(s);
            c.folder = s.text(14);
            out.messages.push_back(std::move(c));
        }
    };
    {
        // En rad extra avslöjar om det finns fler än limit.
        Stmt s(db_, select + " ORDER BY m.modseq, m.id LIMIT ?");
        s.bind(1, accountId).bind(2, static_cast<long long>(since)).bind(3, static_cast<long long>(cutoff)).bind(4, limit + 1);
        readRows(s);
    }
    if (static_cast<int>(out.messages.size()) > limit) {
        out.more = true;
        // Ett nummers rader skrevs i samma transaktion och ska levereras tillsammans: klipp före det sista,
        // påbörjade numret. Är det första numret ensamt större än limit hämtas det i sin helhet.
        std::int64_t boundary = out.messages.back().message.modseq;
        std::int64_t first = out.messages.front().message.modseq;
        if (boundary == first) {
            out.messages.clear();
            Stmt s(db_, select + " AND m.modseq = ? ORDER BY m.id");
            s.bind(1, accountId).bind(2, static_cast<long long>(since)).bind(3, static_cast<long long>(cutoff))
                .bind(4, static_cast<long long>(boundary));
            readRows(s);
            out.cutoff = boundary;
        } else {
            while (!out.messages.empty() && out.messages.back().message.modseq == boundary) out.messages.pop_back();
            out.cutoff = out.messages.back().message.modseq;
        }
    }
    Stmt t(db_, "SELECT f.path, t.uid, t.modseq FROM cache_tombstones t JOIN cache_folders f ON f.id = t.folder_id "
                "WHERE f.account_id = ? AND t.modseq > ? AND t.modseq <= ? ORDER BY t.modseq, t.id");
    t.bind(1, accountId).bind(2, static_cast<long long>(since)).bind(3, static_cast<long long>(out.cutoff));
    while (t.row()) {
        CacheTombstone ts;
        ts.folder = t.text(0);
        ts.uid = static_cast<std::uint32_t>(t.int64(1));
        ts.modseq = t.int64(2);
        out.removed.push_back(std::move(ts));
    }
    return out;
}

void Database::pruneCacheTombstones(std::int64_t olderThan) {
    Lock lock(mutex_);
    writeTx([&] {
        Stmt(db_, "UPDATE cache_seq SET pruned_modseq = GREATEST(pruned_modseq, "
                  "COALESCE((SELECT MAX(modseq) FROM cache_tombstones WHERE created_at < ?), 0)) WHERE id = 1")
            .bind(1, static_cast<long long>(olderThan)).run();
        Stmt(db_, "DELETE FROM cache_tombstones WHERE created_at < ?").bind(1, static_cast<long long>(olderThan)).run();
    });
}

std::optional<CacheMessage> Database::getCacheMessage(long long folderId, std::uint32_t uid) {
    Lock lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kMessageColumns + " FROM cache_messages WHERE folder_id = ? AND uid = ?");
    s.bind(1, folderId).bind(2, static_cast<long long>(uid));
    if (s.row()) return rowToMessage(s);
    return std::nullopt;
}

std::optional<CacheBody> Database::getCacheBody(long long messageRowId) {
    Lock lock(mutex_);
    Stmt s(db_, "SELECT meta, text_z, html_z FROM cache_bodies WHERE message_id = ?");
    s.bind(1, messageRowId);
    if (!s.row()) return std::nullopt;
    CacheBody body;
    body.meta = s.text(0);
    body.text = inflate(s.text(1));
    body.html = inflate(s.text(2));
    return body;
}

CacheProgress Database::cacheProgress(long long accountId) {
    Lock lock(mutex_);
    CacheProgress p;
    {
        Stmt s(db_, "SELECT COUNT(*), COALESCE(SUM(headers_complete), 0) FROM cache_folders WHERE account_id = ? AND "
                    "selectable = 1 AND special_use NOT IN ('all', 'flagged', 'important')");
        s.bind(1, accountId);
        s.row();
        p.folders = s.int64(0);
        p.foldersComplete = s.int64(1);
    }
    Stmt s(db_, "SELECT COUNT(*), COALESCE(SUM(m.body_state != 0), 0) FROM cache_messages m "
                "JOIN cache_folders f ON f.id = m.folder_id WHERE f.account_id = ?");
    s.bind(1, accountId);
    s.row();
    p.messages = s.int64(0);
    p.bodies = s.int64(1);
    return p;
}

std::vector<Account> Database::listAllAccounts() {
    Lock lock(mutex_);
    std::vector<long long> ids;
    {
        Stmt s(db_, "SELECT id FROM accounts ORDER BY id");
        while (s.row()) ids.push_back(s.int64(0));
    }
    std::vector<Account> out;
    for (long long id : ids) {
        if (auto account = getAccount(id)) out.push_back(*account);
    }
    return out;
}

}  // namespace brevduva
