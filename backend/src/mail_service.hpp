#pragma once

#include "db.hpp"
#include "imap.hpp"
#include "mime.hpp"
#include "oauth.hpp"

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
};

struct SendOptions {
    std::optional<MessageRef> replyTo;    // Original som ska märkas som besvarat.
    std::optional<MessageRef> forwardOf;  // Original vars bilagor ska följa med.
    std::optional<MessageRef> draft;      // Utkast som ska tas bort efter att det skickats/ersatts.
    std::optional<long long> identityId;  // Extra avsändare; annars kontots egen adress.
};

class MailService {
public:
    MailService(Database& db, GoogleOAuth& oauth) : db_(db), oauth_(oauth) {}

    // Testar IMAP- och SMTP-inloggning. Returnerar {imap: {ok, error}, smtp: {ok, error}}.
    json testAccount(const Account& account);
    // Testar SMTP-inloggning för en avsändare med egen server.
    json testIdentitySmtp(const Account& account, const Identity& identity);
    void dropSession(long long accountId);

    json listFolders(long long accountId);
    void createFolder(long long accountId, const std::string& path);
    void deleteFolder(long long accountId, const std::string& path);
    void renameFolder(long long accountId, const std::string& from, const std::string& to);

    json listMessages(long long accountId, const MessageQuery& query);
    json getMessage(long long accountId, const MessageRef& ref, bool markSeen);
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
        std::deque<std::pair<std::string, std::string>> rawCache;  // nyckel -> rått meddelande
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

    std::string rawMessage(ImapClient& client, Session& s, const MessageRef& ref);
    std::optional<std::string> specialFolder(ImapClient& client, Session& s, const std::string& use);
    void resolveSpecialFolders(Session& s, const std::vector<FolderInfo>& folders);

    Database& db_;
    GoogleOAuth& oauth_;
    std::mutex sessionsMutex_;
    std::map<long long, std::shared_ptr<Session>> sessions_;
};

}  // namespace brevduva
