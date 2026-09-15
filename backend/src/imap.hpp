#pragma once

#include "net.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tbk {

enum class Security { None, StartTls, Tls };

Security parseSecurity(const std::string& s);
std::string securityName(Security s);

// Ett värde i ett IMAP-svar: atom, sträng (quoted eller literal), NIL eller lista.
struct ImapValue {
    enum class Type { Atom, String, Nil, List };
    Type type = Type::Nil;
    std::string text;
    std::vector<ImapValue> list;

    bool isList() const { return type == Type::List; }
    bool isNil() const { return type == Type::Nil; }
    // Atom eller sträng som text; NIL blir tom sträng.
    const std::string& str() const { return text; }
};

// Tolkar en rå svarsenhet (rad inklusive inbäddade literaler) till värden.
std::vector<ImapValue> parseImapValues(const std::string& raw, std::size_t start = 0);

// Ett IMAP-kommando byggt av text och argument som kvoteras eller skickas som literal.
class ImapCommand {
public:
    explicit ImapCommand(std::string text = {}) { raw(std::move(text)); }
    ImapCommand& raw(const std::string& text);
    ImapCommand& str(const std::string& value);      // Quoted eller literal.
    ImapCommand& literal(const std::string& value);  // Alltid literal.
    ImapCommand& mailbox(const std::string& utf8);   // mUTF-7 + quoted.

    struct Segment {
        std::string text;
        bool literal = false;
    };
    const std::vector<Segment>& segments() const { return segments_; }

private:
    std::vector<Segment> segments_;
};

struct ImapResponse {
    std::vector<std::string> untagged;  // Råa "* ..."-enheter.
    std::string status;                 // OK, NO, BAD
    std::string text;                   // Text efter status.
};

struct FolderInfo {
    std::string path;        // UTF-8
    std::string delimiter;
    std::set<std::string> attributes;  // Gemener, t.ex. "\\noselect", "\\sent"
    std::uint32_t total = 0;
    std::uint32_t unread = 0;
};

struct FetchedMessage {
    std::uint32_t uid = 0;
    std::set<std::string> flags;
    std::string internalDate;
    std::uint64_t size = 0;
    std::string body;  // Header-fält eller hela meddelandet beroende på fråga.
};

class ImapClient {
public:
    ImapClient() = default;

    void connect(const std::string& host, int port, Security security, bool tlsVerify);
    void login(const std::string& user, const std::string& password);
    // SASL XOAUTH2 med en OAuth 2.0 access-token (Gmail).
    void loginXOAuth2(const std::string& user, const std::string& accessToken);
    void logout();
    bool connected() const { return conn_.isOpen(); }
    bool hasCapability(const std::string& cap) const;

    ImapResponse exec(const ImapCommand& cmd);
    ImapResponse execOk(const ImapCommand& cmd);

    std::vector<FolderInfo> listFolders(bool withStatus = true);
    // Väljer mappen om den inte redan är vald. Returnerar antal meddelanden.
    std::uint32_t select(const std::string& folder);
    void noop() { execOk(ImapCommand("NOOP")); }

    std::vector<std::uint32_t> uidSearch(const ImapCommand& criteria, bool utf8);
    std::vector<FetchedMessage> uidFetch(const std::string& uidSet, const std::string& items);
    void uidStore(const std::string& uidSet, const std::string& op, const std::string& flags);
    void uidMove(const std::string& uidSet, const std::string& target);
    void expungeUids(const std::string& uidSet);
    std::optional<std::uint32_t> append(const std::string& folder, const std::string& flags,
                                        const std::string& message);
    void createFolder(const std::string& folder);
    void deleteFolder(const std::string& folder);
    void renameFolder(const std::string& from, const std::string& to);

    static std::string uidSetFrom(std::vector<std::uint32_t> uids);

private:
    ImapResponse authenticate(const std::string& mechanism, const std::string& initialResponse);
    std::string readUnit();
    void readGreeting();
    void refreshCapabilities();
    std::string nextTag();

    Connection conn_;
    std::set<std::string> caps_;
    std::optional<std::string> selected_;
    std::uint32_t tagCounter_ = 0;
};

}  // namespace tbk
