#pragma once

#include "db.hpp"
#include "imap.hpp"
#include "mime.hpp"
#include "oauth.hpp"
#include "raw_cache.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace brevduva {

using json = nlohmann::json;

struct MessageRef {
    std::string folder;
    std::uint32_t uid = 0;
};

struct MessageQuery {
    std::string folder = "INBOX";
    int page = 0;
    int pageSize = 50;
    std::string text;
    bool unreadOnly = false;
    bool flaggedOnly = false;
    std::uint32_t beforeUid = 0;  // Bara meddelanden med lägre UID, för att gå igenom en mapp bakåt. 0 = alla.
    // Sortering: "uid" (ankomstordning), "date", "subject", "from", "to" eller "size". Webbgränssnittet skickar
    // "date"; "uid" används internt när en mapp gås igenom bakåt med beforeUid.
    std::string sort = "uid";
    bool descending = true;
};

// De sorteringar som listor och sökning tar emot.
inline bool isSortKey(const std::string& key) {
    return key == "uid" || key == "date" || key == "subject" || key == "from" || key == "to" || key == "size";
}

struct SendOptions {
    std::optional<MessageRef> replyTo;    // Original som ska märkas som besvarat.
    std::optional<MessageRef> forwardOf;  // Original vars bilagor ska följa med.
    std::optional<MessageRef> draft;      // Utkast som ska tas bort efter att det skickats/ersatts.
    std::optional<long long> identityId;  // Extra avsändare; annars kontots egen adress.
};

// Underlag för AI-sortering: rubriker och början av meddelandets text.
struct MessageSummary {
    std::uint32_t uid = 0;
    std::string subject;
    std::vector<mime::Address> from, to;
    std::int64_t date = 0;
    bool seen = false;
    std::string text;
    std::string html;
    std::string messageId;
};

// Meddelanden som kommit till en mapp efter ett visst UID.
struct NewMessages {
    std::uint32_t uidValidity = 0;
    std::uint32_t uidNext = 0;
    std::vector<std::uint32_t> uids;  // Stigande.
};

// Mappens specialanvändning ("inbox", "sent", "drafts", "trash", "junk", "archive", "all", "flagged") eller tom.
std::string specialUseOf(const FolderInfo& folder);

class MailCache;

class MailService {
public:
    MailService(Database& db, GoogleOAuth& oauth, RawCache& rawCache) : db_(db), oauth_(oauth), rawCache_(rawCache) {}
    // Med en mejlcache läses mappar, listor och meddelanden därifrån när det går, och ändringar förs in i den.
    void setCache(MailCache* cache) { cache_ = cache; }

    // Testar IMAP- och SMTP-inloggning. Returnerar {imap: {ok, error}, smtp: {ok, error}}.
    json testAccount(const Account& account);
    // Testar SMTP-inloggning för en avsändare med egen server.
    json testIdentitySmtp(const Account& account, const Identity& identity);
    void dropSession(long long accountId);
    // En ny, inloggad IMAP-anslutning som inte delas med webbgränssnittet, för långa jobb som backup.
    std::unique_ptr<ImapClient> connectImap(const Account& account);

    // withStatus = false hoppar över antal meddelanden och olästa, vilket går fortare.
    json listFolders(long long accountId, bool withStatus = true);
    void createFolder(long long accountId, const std::string& path);
    void deleteFolder(long long accountId, const std::string& path);
    void renameFolder(long long accountId, const std::string& from, const std::string& to);

    json listMessages(long long accountId, const MessageQuery& query);
    json getMessage(long long accountId, const MessageRef& ref, bool markSeen);
    // Hämtar rubriker och högst bodyBytes av varje meddelandes innehåll, så att stora bilagor inte laddas ned.
    // Meddelandena märks inte som lästa.
    std::vector<MessageSummary> summarize(long long accountId, const std::string& folder,
                                          const std::vector<std::uint32_t>& uids, std::size_t bodyBytes);
    // UID:n större än afterUid, högst limit stycken med de äldsta först.
    NewMessages newMessages(long long accountId, const std::string& folder, std::uint32_t afterUid, std::size_t limit);
    // Flyttar meddelandet med ett visst Message-ID. Returnerar false om det inte finns i mappen.
    bool moveByMessageId(long long accountId, const std::string& folder, const std::string& messageId,
                         const std::string& target);
    mime::Attachment getAttachment(long long accountId, const MessageRef& ref, int index);
    std::string getSource(long long accountId, const MessageRef& ref);

    void setFlags(long long accountId, const std::string& folder, const std::vector<std::uint32_t>& uids,
                  const std::vector<std::string>& add, const std::vector<std::string>& remove);
    void moveMessages(long long accountId, const std::string& folder, const std::vector<std::uint32_t>& uids,
                      const std::string& target);
    void deleteMessages(long long accountId, const std::string& folder, const std::vector<std::uint32_t>& uids);

    json send(long long accountId, mime::OutgoingMessage message, const SendOptions& options);
    json saveDraft(long long accountId, mime::OutgoingMessage message, const SendOptions& options);

private:
    struct Session {
        std::mutex mutex;
        std::unique_ptr<ImapClient> client;
        std::chrono::steady_clock::time_point lastUse;
        std::map<std::string, std::string> specialFolders;  // "sent" -> sökväg
        bool specialResolved = false;
    };

    std::shared_ptr<Session> session(long long accountId);
    Account account(long long accountId);
    // Loggar in med lösenord eller Google XOAUTH2 beroende på kontot.
    void login(ImapClient& client, const Account& acc);
    // Kopia av kontot med en giltig access-token om det använder Google-inloggning.
    Account withAccessToken(Account acc);
    // Sätter avsändare och svarsadress, och returnerar de SMTP-inställningar som ska användas.
    Account applySender(const Account& acc, mime::OutgoingMessage& message, const SendOptions& options);

    template <typename F>
    auto withImap(long long accountId, F&& fn);

    json listFoldersLive(long long accountId, bool withStatus);
    std::string rawMessage(long long accountId, ImapClient& client, const MessageRef& ref);
    std::string rawMessageCached(long long accountId, const MessageRef& ref);
    json listMessagesLive(long long accountId, const MessageQuery& query);
    json getMessageLive(long long accountId, const MessageRef& ref, bool markSeen);
    std::optional<std::string> specialFolder(ImapClient& client, Session& s, const std::string& use);
    void resolveSpecialFolders(Session& s, const std::vector<FolderInfo>& folders);

    Database& db_;
    GoogleOAuth& oauth_;
    RawCache& rawCache_;
    MailCache* cache_ = nullptr;
    std::mutex sessionsMutex_;
    std::map<long long, std::shared_ptr<Session>> sessions_;
};

}  // namespace brevduva
