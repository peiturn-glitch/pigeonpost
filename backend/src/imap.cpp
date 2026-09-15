#include "imap.hpp"

#include "errors.hpp"
#include "mutf7.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace brevduva {

namespace {

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string base64(const std::string& in) {
    std::string out(4 * ((in.size() + 2) / 3), '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(in.data()),
                            static_cast<int>(in.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

// Returnerar literal-längden om raden slutar med "{n}\r\n" eller "{n+}\r\n".
std::optional<std::size_t> trailingLiteral(const std::string& line) {
    std::size_t end = line.size();
    if (end >= 2 && line[end - 2] == '\r' && line[end - 1] == '\n') end -= 2;
    else if (end >= 1 && line[end - 1] == '\n') end -= 1;
    if (end == 0 || line[end - 1] != '}') return std::nullopt;
    std::size_t open = line.rfind('{', end - 1);
    if (open == std::string::npos) return std::nullopt;
    std::string num = line.substr(open + 1, end - 1 - open - 1);
    if (!num.empty() && num.back() == '+') num.pop_back();
    if (num.empty() || !std::all_of(num.begin(), num.end(), ::isdigit)) return std::nullopt;
    return static_cast<std::size_t>(std::stoull(num));
}

bool needsLiteral(const std::string& s) {
    if (s.size() > 1024) return true;
    for (unsigned char c : s) {
        if (c == 0 || c == '\r' || c == '\n' || c >= 0x80) return true;
    }
    return false;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

class Parser {
public:
    Parser(const std::string& s, std::size_t pos) : s_(s), pos_(pos) {}

    std::vector<ImapValue> parseAll() {
        std::vector<ImapValue> out;
        while (true) {
            skipSpaces();
            if (pos_ >= s_.size() || s_[pos_] == '\r' || s_[pos_] == '\n') break;
            if (s_[pos_] == ')') { ++pos_; continue; }  // Obalanserat; ignorera.
            out.push_back(parseValue());
        }
        return out;
    }

private:
    void skipSpaces() {
        while (pos_ < s_.size() && s_[pos_] == ' ') ++pos_;
    }

    ImapValue parseValue() {
        char c = s_[pos_];
        if (c == '(') return parseList();
        if (c == '"') return parseQuoted();
        if (c == '{') {
            if (auto lit = tryLiteral()) return *lit;
        }
        return parseAtom();
    }

    ImapValue parseList() {
        ImapValue v;
        v.type = ImapValue::Type::List;
        ++pos_;
        while (true) {
            skipSpaces();
            if (pos_ >= s_.size()) break;
            char c = s_[pos_];
            if (c == ')') { ++pos_; break; }
            if (c == '\r' || c == '\n') break;
            v.list.push_back(parseValue());
        }
        return v;
    }

    ImapValue parseQuoted() {
        ImapValue v;
        v.type = ImapValue::Type::String;
        ++pos_;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) ++pos_;
            v.text += s_[pos_++];
        }
        if (pos_ < s_.size()) ++pos_;
        return v;
    }

    std::optional<ImapValue> tryLiteral() {
        std::size_t close = s_.find('}', pos_);
        if (close == std::string::npos) return std::nullopt;
        std::string num = s_.substr(pos_ + 1, close - pos_ - 1);
        if (!num.empty() && num.back() == '+') num.pop_back();
        if (num.empty() || !std::all_of(num.begin(), num.end(), ::isdigit)) return std::nullopt;
        std::size_t n = std::stoull(num);
        std::size_t dataStart = close + 1;
        if (dataStart < s_.size() && s_[dataStart] == '\r') ++dataStart;
        if (dataStart < s_.size() && s_[dataStart] == '\n') ++dataStart;
        ImapValue v;
        v.type = ImapValue::Type::String;
        v.text = s_.substr(dataStart, n);
        pos_ = std::min(s_.size(), dataStart + n);
        return v;
    }

    ImapValue parseAtom() {
        ImapValue v;
        v.type = ImapValue::Type::Atom;
        int bracketDepth = 0;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (bracketDepth == 0 && (c == ' ' || c == '(' || c == ')' || c == '\r' || c == '\n')) {
                break;
            }
            if (c == '[') ++bracketDepth;
            if (c == ']' && bracketDepth > 0) --bracketDepth;
            v.text += c;
            ++pos_;
        }
        if (v.text.empty() && pos_ < s_.size()) v.text += s_[pos_++];  // Undvik oändlig loop.
        if (v.text == "NIL") {
            v.type = ImapValue::Type::Nil;
            v.text.clear();
        }
        return v;
    }

    const std::string& s_;
    std::size_t pos_;
};

// Hämtar de råa enheter som börjar med "* <keyword>" eller "* <n> <keyword>".
std::vector<ImapValue> valuesAfterKeyword(const std::string& unit, const std::string& keyword,
                                          bool numbered) {
    auto values = parseImapValues(unit, 0);
    std::size_t idx = numbered ? 2 : 1;
    if (values.size() <= idx || values[0].text != "*") return {};
    if (toUpper(values[idx].text) != keyword) return {};
    return {values.begin() + static_cast<long>(idx) + 1, values.end()};
}

}  // namespace

Security parseSecurity(const std::string& s) {
    std::string l = toLower(s);
    if (l == "tls" || l == "ssl") return Security::Tls;
    if (l == "starttls") return Security::StartTls;
    return Security::None;
}

std::string securityName(Security s) {
    switch (s) {
        case Security::Tls: return "tls";
        case Security::StartTls: return "starttls";
        default: return "none";
    }
}

std::vector<ImapValue> parseImapValues(const std::string& raw, std::size_t start) {
    return Parser(raw, start).parseAll();
}

ImapCommand& ImapCommand::raw(const std::string& text) {
    if (text.empty()) return *this;
    if (!segments_.empty() && !segments_.back().literal) {
        segments_.back().text += text;
    } else {
        segments_.push_back({text, false});
    }
    return *this;
}

ImapCommand& ImapCommand::str(const std::string& value) {
    if (needsLiteral(value)) {
        segments_.push_back({value, true});
    } else {
        raw(quote(value));
    }
    return *this;
}

ImapCommand& ImapCommand::literal(const std::string& value) {
    segments_.push_back({value, true});
    return *this;
}

ImapCommand& ImapCommand::mailbox(const std::string& utf8) {
    return str(utf8ToMutf7(utf8));
}

// ---------------------------------------------------------------------------

std::string ImapClient::nextTag() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "T%04u", ++tagCounter_);
    return buf;
}

std::string ImapClient::readUnit() {
    std::string unit = conn_.readLine();
    std::string lastLine = unit;
    while (auto n = trailingLiteral(lastLine)) {
        unit += conn_.readBytes(*n);
        lastLine = conn_.readLine();
        unit += lastLine;
    }
    return unit;
}

void ImapClient::readGreeting() {
    std::string greeting = readUnit();
    if (greeting.rfind("* BYE", 0) == 0) {
        conn_.close();
        throw MailServerError("Servern avvisade anslutningen: " + greeting);
    }
    if (greeting.rfind("* OK", 0) != 0 && greeting.rfind("* PREAUTH", 0) != 0) {
        conn_.close();
        throw NetError("Oväntat svar från IMAP-servern: " + greeting.substr(0, 200));
    }
}

void ImapClient::connect(const std::string& host, int port, Security security, bool tlsVerify) {
    caps_.clear();
    selected_.reset();
    conn_.connect(host, port);
    if (security == Security::Tls) conn_.startTls(host, tlsVerify);
    readGreeting();
    refreshCapabilities();

    if (security == Security::StartTls) {
        if (!hasCapability("STARTTLS")) {
            conn_.close();
            throw MailServerError("IMAP-servern stöder inte STARTTLS");
        }
        execOk(ImapCommand("STARTTLS"));
        conn_.startTls(host, tlsVerify);
        refreshCapabilities();
    }
}

void ImapClient::refreshCapabilities() {
    auto resp = execOk(ImapCommand("CAPABILITY"));
    caps_.clear();
    for (const auto& unit : resp.untagged) {
        for (const auto& v : valuesAfterKeyword(unit, "CAPABILITY", false)) {
            caps_.insert(toUpper(v.text));
        }
    }
}

bool ImapClient::hasCapability(const std::string& cap) const {
    return caps_.count(toUpper(cap)) > 0;
}

ImapResponse ImapClient::authenticate(const std::string& mechanism, const std::string& initialResponse) {
    std::string tag = nextTag();
    bool sent = hasCapability("SASL-IR");
    conn_.write(tag + " AUTHENTICATE " + mechanism + (sent ? " " + initialResponse : "") + "\r\n");

    ImapResponse resp;
    while (true) {
        std::string line = readUnit();
        if (line.rfind("+", 0) == 0) {
            // Första fortsättningen begär svaret; en senare innehåller ett felmeddelande (t.ex. XOAUTH2)
            // som klienten ska kvittera med en tom rad för att få det taggade NO-svaret.
            conn_.write(sent ? "\r\n" : initialResponse + "\r\n");
            sent = true;
        } else if (line.rfind(tag + " ", 0) == 0) {
            std::string rest = line.substr(tag.size() + 1);
            std::size_t sp = rest.find(' ');
            resp.status = toUpper(rest.substr(0, sp));
            resp.text = sp == std::string::npos ? "" : rest.substr(sp + 1);
            while (!resp.text.empty() && (resp.text.back() == '\n' || resp.text.back() == '\r')) resp.text.pop_back();
            return resp;
        } else if (line.rfind("* ", 0) == 0) {
            resp.untagged.push_back(std::move(line));
        }
    }
}

void ImapClient::login(const std::string& user, const std::string& password) {
    ImapResponse resp;
    if (hasCapability("LOGINDISABLED") && hasCapability("AUTH=PLAIN")) {
        resp = authenticate("PLAIN", base64(std::string("\0", 1) + user + std::string("\0", 1) + password));
    } else {
        resp = exec(ImapCommand("LOGIN ").str(user).raw(" ").str(password));
    }
    if (resp.status != "OK") {
        throw MailServerError("Inloggningen avvisades av IMAP-servern: " + resp.text);
    }
    refreshCapabilities();
}

void ImapClient::loginXOAuth2(const std::string& user, const std::string& accessToken) {
    std::string payload = "user=" + user + "\x01" + "auth=Bearer " + accessToken + "\x01\x01";
    ImapResponse resp = authenticate("XOAUTH2", base64(payload));
    if (resp.status != "OK") {
        throw MailServerError("Google-inloggningen avvisades av IMAP-servern: " + resp.text +
                              ". Logga in med Google igen i kontoinställningarna.");
    }
    refreshCapabilities();
}

void ImapClient::logout() {
    if (!conn_.isOpen()) return;
    try {
        exec(ImapCommand("LOGOUT"));
    } catch (...) {
    }
    conn_.close();
    selected_.reset();
}

ImapResponse ImapClient::exec(const ImapCommand& cmd) {
    if (!conn_.isOpen()) throw NetError("Inte ansluten till IMAP-servern");

    std::string tag = nextTag();
    bool literalPlus = hasCapability("LITERAL+");
    ImapResponse resp;

    auto readTagged = [&](bool waitForContinuation) -> bool {
        // Returnerar true om en fortsättning ("+") togs emot, false om taggat svar.
        while (true) {
            std::string unit = readUnit();
            if (unit.rfind("* ", 0) == 0) {
                resp.untagged.push_back(std::move(unit));
            } else if (unit.rfind("+", 0) == 0) {
                if (waitForContinuation) return true;
            } else if (unit.rfind(tag + " ", 0) == 0) {
                std::string rest = unit.substr(tag.size() + 1);
                std::size_t sp = rest.find(' ');
                resp.status = toUpper(rest.substr(0, sp));
                resp.text = sp == std::string::npos ? "" : rest.substr(sp + 1);
                while (!resp.text.empty() &&
                       (resp.text.back() == '\n' || resp.text.back() == '\r')) {
                    resp.text.pop_back();
                }
                return false;
            }
        }
    };

    std::string pending = tag + " ";
    for (const auto& seg : cmd.segments()) {
        if (!seg.literal) {
            pending += seg.text;
            continue;
        }
        if (literalPlus) {
            pending += "{" + std::to_string(seg.text.size()) + "+}\r\n" + seg.text;
            continue;
        }
        pending += "{" + std::to_string(seg.text.size()) + "}\r\n";
        conn_.write(pending);
        pending.clear();
        if (!readTagged(true)) return resp;  // Servern avvisade literalen.
        conn_.write(seg.text);
    }
    pending += "\r\n";
    conn_.write(pending);
    readTagged(false);
    return resp;
}

ImapResponse ImapClient::execOk(const ImapCommand& cmd) {
    ImapResponse resp = exec(cmd);
    if (resp.status != "OK") {
        throw MailServerError("IMAP-fel: " + (resp.text.empty() ? resp.status : resp.text));
    }
    return resp;
}

std::vector<FolderInfo> ImapClient::listFolders(bool withStatus) {
    ImapCommand cmd("LIST \"\" \"*\"");
    if (hasCapability("SPECIAL-USE")) cmd.raw(" RETURN (SPECIAL-USE)");
    auto resp = execOk(cmd);

    std::vector<FolderInfo> folders;
    for (const auto& unit : resp.untagged) {
        auto values = valuesAfterKeyword(unit, "LIST", false);
        if (values.size() < 3 || !values[0].isList()) continue;
        FolderInfo f;
        for (const auto& attr : values[0].list) f.attributes.insert(toLower(attr.text));
        f.delimiter = values[1].isNil() ? "" : values[1].text;
        f.path = mutf7ToUtf8(values[2].text);
        if (toUpper(f.path) == "INBOX") f.path = "INBOX";
        folders.push_back(std::move(f));
    }

    // STATUS skickas i pipeline för att slippa en rundtur per mapp.
    std::map<std::string, std::size_t> byTag;
    std::string batch;
    std::vector<std::string> tags;
    for (std::size_t i = 0; withStatus && i < folders.size(); ++i) {
        if (folders[i].attributes.count("\\noselect") || folders[i].attributes.count("\\nonexistent")) {
            continue;
        }
        std::string tag = nextTag();
        byTag[tag] = i;
        tags.push_back(tag);
        batch += tag + " STATUS " + quote(utf8ToMutf7(folders[i].path)) + " (MESSAGES UNSEEN)\r\n";
    }
    if (!tags.empty()) {
        conn_.write(batch);
        std::size_t remaining = tags.size();
        std::map<std::string, std::size_t> byName;
        for (std::size_t i = 0; i < folders.size(); ++i) byName[utf8ToMutf7(folders[i].path)] = i;
        while (remaining > 0) {
            std::string unit = readUnit();
            if (unit.rfind("* ", 0) == 0) {
                auto values = valuesAfterKeyword(unit, "STATUS", false);
                if (values.size() >= 2 && values[1].isList()) {
                    std::string name = values[0].text;
                    auto it = byName.find(name);
                    if (it == byName.end() && toUpper(name) == "INBOX") it = byName.find("INBOX");
                    if (it == byName.end()) continue;
                    const auto& items = values[1].list;
                    for (std::size_t k = 0; k + 1 < items.size(); k += 2) {
                        std::string key = toUpper(items[k].text);
                        std::uint32_t val = static_cast<std::uint32_t>(std::stoul(items[k + 1].text));
                        if (key == "MESSAGES") folders[it->second].total = val;
                        if (key == "UNSEEN") folders[it->second].unread = val;
                    }
                }
            } else {
                std::size_t sp = unit.find(' ');
                if (sp != std::string::npos && byTag.count(unit.substr(0, sp))) --remaining;
            }
        }
    }
    return folders;
}

std::uint32_t ImapClient::select(const std::string& folder) {
    auto resp = exec(ImapCommand("SELECT ").mailbox(folder));
    if (resp.status != "OK") {
        selected_.reset();
        throw MailServerError("Kunde inte öppna mappen " + folder + ": " + resp.text);
    }
    selected_ = folder;
    std::uint32_t exists = 0;
    for (const auto& unit : resp.untagged) {
        auto values = parseImapValues(unit, 0);
        if (values.size() >= 3 && toUpper(values[2].text) == "EXISTS") {
            exists = static_cast<std::uint32_t>(std::stoul(values[1].text));
        }
    }
    return exists;
}

std::vector<std::uint32_t> ImapClient::uidSearch(const ImapCommand& criteria, bool utf8) {
    ImapCommand cmd(utf8 ? "UID SEARCH CHARSET UTF-8 " : "UID SEARCH ");
    for (const auto& seg : criteria.segments()) {
        if (seg.literal) cmd.literal(seg.text);
        else cmd.raw(seg.text);
    }
    auto resp = execOk(cmd);
    std::vector<std::uint32_t> uids;
    for (const auto& unit : resp.untagged) {
        for (const auto& v : valuesAfterKeyword(unit, "SEARCH", false)) {
            if (!v.text.empty() && std::all_of(v.text.begin(), v.text.end(), ::isdigit)) {
                uids.push_back(static_cast<std::uint32_t>(std::stoul(v.text)));
            }
        }
    }
    return uids;
}

std::vector<FetchedMessage> ImapClient::uidFetch(const std::string& uidSet, const std::string& items) {
    auto resp = execOk(ImapCommand("UID FETCH " + uidSet + " " + items));
    std::vector<FetchedMessage> out;
    for (const auto& unit : resp.untagged) {
        auto values = valuesAfterKeyword(unit, "FETCH", true);
        if (values.empty() || !values[0].isList()) continue;
        const auto& attrs = values[0].list;
        FetchedMessage m;
        for (std::size_t k = 0; k + 1 < attrs.size(); k += 2) {
            std::string key = toUpper(attrs[k].text);
            const ImapValue& val = attrs[k + 1];
            if (key == "UID") {
                m.uid = static_cast<std::uint32_t>(std::stoul(val.text));
            } else if (key == "FLAGS") {
                for (const auto& f : val.list) m.flags.insert(toLower(f.text));
            } else if (key == "INTERNALDATE") {
                m.internalDate = val.text;
            } else if (key == "RFC822.SIZE") {
                m.size = std::stoull(val.text);
            } else if (key.rfind("BODY[", 0) == 0) {
                m.body = val.text;
            }
        }
        if (m.uid != 0) out.push_back(std::move(m));
    }
    return out;
}

void ImapClient::uidStore(const std::string& uidSet, const std::string& op, const std::string& flags) {
    execOk(ImapCommand("UID STORE " + uidSet + " " + op + " (" + flags + ")"));
}

void ImapClient::uidMove(const std::string& uidSet, const std::string& target) {
    if (hasCapability("MOVE")) {
        execOk(ImapCommand("UID MOVE " + uidSet + " ").mailbox(target));
        return;
    }
    execOk(ImapCommand("UID COPY " + uidSet + " ").mailbox(target));
    uidStore(uidSet, "+FLAGS.SILENT", "\\Deleted");
    expungeUids(uidSet);
}

void ImapClient::expungeUids(const std::string& uidSet) {
    if (hasCapability("UIDPLUS")) {
        execOk(ImapCommand("UID EXPUNGE " + uidSet));
    } else {
        execOk(ImapCommand("EXPUNGE"));
    }
}

std::optional<std::uint32_t> ImapClient::append(const std::string& folder, const std::string& flags,
                                                const std::string& message) {
    ImapCommand cmd("APPEND ");
    cmd.mailbox(folder);
    if (!flags.empty()) cmd.raw(" (" + flags + ")");
    cmd.raw(" ").literal(message);

    auto resp = execOk(cmd);
    // Tagged OK kan innehålla [APPENDUID <uidvalidity> <uid>].
    std::string upper = toUpper(resp.text);
    std::size_t pos = upper.find("[APPENDUID ");
    if (pos != std::string::npos) {
        std::size_t a = resp.text.find(' ', pos + 11);
        std::size_t b = resp.text.find(']', a);
        if (a != std::string::npos && b != std::string::npos) {
            try {
                return static_cast<std::uint32_t>(std::stoul(resp.text.substr(a + 1, b - a - 1)));
            } catch (...) {
            }
        }
    }
    return std::nullopt;
}

void ImapClient::createFolder(const std::string& folder) {
    execOk(ImapCommand("CREATE ").mailbox(folder));
    exec(ImapCommand("SUBSCRIBE ").mailbox(folder));
}

void ImapClient::deleteFolder(const std::string& folder) {
    if (selected_ == folder) {
        exec(ImapCommand(hasCapability("UNSELECT") ? "UNSELECT" : "CLOSE"));
        selected_.reset();
    }
    exec(ImapCommand("UNSUBSCRIBE ").mailbox(folder));
    execOk(ImapCommand("DELETE ").mailbox(folder));
}

void ImapClient::renameFolder(const std::string& from, const std::string& to) {
    if (selected_ == from) {
        exec(ImapCommand(hasCapability("UNSELECT") ? "UNSELECT" : "CLOSE"));
        selected_.reset();
    }
    execOk(ImapCommand("RENAME ").mailbox(from).raw(" ").mailbox(to));
    exec(ImapCommand("SUBSCRIBE ").mailbox(to));
}

std::string ImapClient::uidSetFrom(std::vector<std::uint32_t> uids) {
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
    std::string out;
    for (std::size_t i = 0; i < uids.size();) {
        std::size_t j = i;
        while (j + 1 < uids.size() && uids[j + 1] == uids[j] + 1) ++j;
        if (!out.empty()) out += ',';
        out += std::to_string(uids[i]);
        if (j > i) out += ":" + std::to_string(uids[j]);
        i = j + 1;
    }
    return out;
}

}  // namespace brevduva
