#include "thunderbird_prefs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <set>

namespace tbk::thunderbird {

namespace {

// Thunderbirds konstanter (MailNewsTypes2.idl).
constexpr int kSocketPlain = 0;
constexpr int kSocketTrySTARTTLS = 1;  // Föråldrad; tolkas som STARTTLS.
constexpr int kSocketSTARTTLS = 2;
constexpr int kSocketSSL = 3;
constexpr int kAuthNone = 1;
constexpr int kAuthPasswordCleartext = 3;
constexpr int kAuthOAuth2 = 10;

// --- Tolkning av prefs.js -------------------------------------------------------

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

class PrefsParser {
public:
    explicit PrefsParser(const std::string& s) : s_(s) {}

    Prefs parse() {
        Prefs prefs;
        while (true) {
            skipSpaceAndComments();
            if (pos_ >= s_.size()) break;
            std::size_t start = pos_;
            if (!parseEntry(prefs)) {
                // Hoppa till nästa sats vid syntaxfel.
                pos_ = start;
                std::size_t semi = s_.find(';', pos_);
                pos_ = semi == std::string::npos ? s_.size() : semi + 1;
            }
        }
        return prefs;
    }

private:
    void skipSpaceAndComments() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == '#' || (c == '/' && peek(1) == '/')) {
                std::size_t nl = s_.find('\n', pos_);
                pos_ = nl == std::string::npos ? s_.size() : nl + 1;
            } else if (c == '/' && peek(1) == '*') {
                std::size_t end = s_.find("*/", pos_ + 2);
                pos_ = end == std::string::npos ? s_.size() : end + 2;
            } else {
                break;
            }
        }
    }

    char peek(std::size_t offset) const {
        return pos_ + offset < s_.size() ? s_[pos_ + offset] : '\0';
    }

    bool expect(char c) {
        skipSpaceAndComments();
        if (peek(0) != c) return false;
        ++pos_;
        return true;
    }

    bool parseEntry(Prefs& prefs) {
        std::size_t idStart = pos_;
        while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
        std::string ident = s_.substr(idStart, pos_ - idStart);
        if (ident != "user_pref" && ident != "pref" && ident != "sticky_pref") return false;
        if (!expect('(')) return false;
        std::string name;
        if (!parseString(name)) return false;
        if (!expect(',')) return false;
        skipSpaceAndComments();

        PrefValue value;
        char c = peek(0);
        if (c == '"' || c == '\'') {
            value.type = PrefValue::Type::String;
            if (!parseString(value.str)) return false;
        } else if (s_.compare(pos_, 4, "true") == 0) {
            value.type = PrefValue::Type::Bool;
            value.boolean = true;
            pos_ += 4;
        } else if (s_.compare(pos_, 5, "false") == 0) {
            value.type = PrefValue::Type::Bool;
            pos_ += 5;
        } else if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            value.type = PrefValue::Type::Int;
            std::size_t numStart = pos_;
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
            try {
                value.num = std::stoll(s_.substr(numStart, pos_ - numStart));
            } catch (...) {
                return false;
            }
        } else {
            return false;
        }

        // pref() kan ha attribut som "sticky" efter värdet; de ignoreras.
        skipSpaceAndComments();
        while (peek(0) == ',') {
            ++pos_;
            skipSpaceAndComments();
            while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
            skipSpaceAndComments();
        }
        if (!expect(')')) return false;
        if (!expect(';')) return false;
        prefs[name] = std::move(value);
        return true;
    }

    int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool readHex(std::size_t digits, char32_t& out) {
        out = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            int v = hexValue(peek(0));
            if (v < 0) return false;
            out = (out << 4) | static_cast<char32_t>(v);
            ++pos_;
        }
        return true;
    }

    bool parseString(std::string& out) {
        skipSpaceAndComments();
        char quote = peek(0);
        if (quote != '"' && quote != '\'') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == quote) return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            char e = peek(0);
            ++pos_;
            char32_t cp = 0;
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'x':
                    if (!readHex(2, cp)) return false;
                    appendUtf8(out, cp);
                    break;
                case 'u':
                    if (!readHex(4, cp)) return false;
                    // Surrogatpar skrivs som två \u-sekvenser.
                    if (cp >= 0xd800 && cp < 0xdc00 && peek(0) == '\\' && peek(1) == 'u') {
                        std::size_t save = pos_;
                        pos_ += 2;
                        char32_t low = 0;
                        if (readHex(4, low) && low >= 0xdc00 && low < 0xe000) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        } else {
                            pos_ = save;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                default: out += e; break;
            }
        }
        return false;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

// --- Hjälpfunktioner för import --------------------------------------------------

std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t comma = value.find(',', start);
        std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class PrefReader {
public:
    explicit PrefReader(const Prefs& prefs) : prefs_(prefs) {}

    std::string str(const std::string& name, const std::string& fallback = {}) const {
        auto it = prefs_.find(name);
        if (it == prefs_.end()) return fallback;
        if (it->second.type == PrefValue::Type::String) return it->second.str;
        if (it->second.type == PrefValue::Type::Int) return std::to_string(it->second.num);
        return it->second.boolean ? "true" : "false";
    }

    long long num(const std::string& name, long long fallback) const {
        auto it = prefs_.find(name);
        if (it == prefs_.end()) return fallback;
        if (it->second.type == PrefValue::Type::Int) return it->second.num;
        if (it->second.type == PrefValue::Type::String) {
            try {
                return std::stoll(it->second.str);
            } catch (...) {
            }
        }
        return fallback;
    }

    bool boolean(const std::string& name, bool fallback) const {
        auto it = prefs_.find(name);
        if (it == prefs_.end()) return fallback;
        if (it->second.type == PrefValue::Type::Bool) return it->second.boolean;
        if (it->second.type == PrefValue::Type::String) return it->second.str == "true";
        return it->second.num != 0;
    }

private:
    const Prefs& prefs_;
};

std::string securityFromSocketType(long long socketType) {
    switch (socketType) {
        case kSocketSSL: return "tls";
        case kSocketSTARTTLS:
        case kSocketTrySTARTTLS: return "starttls";
        default: return "none";
    }
}

int socketTypeFromSecurity(const std::string& security) {
    if (security == "tls") return kSocketSSL;
    if (security == "starttls") return kSocketSTARTTLS;
    return kSocketPlain;
}

// Thunderbirds signaturer kan vara HTML; vi lagrar ren text.
std::string htmlToText(const std::string& html) {
    std::string out;
    std::size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            std::size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = lower(html.substr(i + 1, end - i - 1));
            if (tag.rfind("br", 0) == 0 || tag == "/p" || tag == "/div" || tag == "/li") out += '\n';
            i = end + 1;
        } else if (html[i] == '&') {
            std::size_t end = html.find(';', i);
            if (end == std::string::npos || end - i > 10) {
                out += html[i++];
                continue;
            }
            std::string entity = html.substr(i + 1, end - i - 1);
            if (entity == "amp") out += '&';
            else if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (entity == "nbsp") out += ' ';
            else if (!entity.empty() && entity[0] == '#') {
                try {
                    char32_t cp = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                                      ? static_cast<char32_t>(std::stoul(entity.substr(2), nullptr, 16))
                                      : static_cast<char32_t>(std::stoul(entity.substr(1)));
                    appendUtf8(out, cp);
                } catch (...) {
                }
            } else {
                out += html.substr(i, end - i + 1);
            }
            i = end + 1;
        } else {
            out += html[i++];
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

struct SmtpSettings {
    std::string host;
    int port = 0;
    std::string security = "none";
    std::string username;
    bool noAuth = false;
    bool found = false;
};

SmtpSettings readSmtp(const PrefReader& r, const std::string& key) {
    SmtpSettings s;
    if (key.empty()) return s;
    std::string prefix = "mail.smtpserver." + key + ".";
    s.host = r.str(prefix + "hostname");
    if (s.host.empty()) return s;
    s.found = true;
    long long socketType = r.num(prefix + "try_ssl", kSocketPlain);
    s.security = securityFromSocketType(socketType);
    // Thunderbird: port 0 betyder standard (465 för SSL/TLS, annars 25).
    long long port = r.num(prefix + "port", 0);
    s.port = static_cast<int>(port > 0 ? port : (socketType == kSocketSSL ? 465 : 25));
    s.username = r.str(prefix + "username");
    s.noAuth = r.num(prefix + "authMethod", kAuthPasswordCleartext) == kAuthNone;
    return s;
}

std::string signatureOf(const PrefReader& r, const std::string& prefix) {
    std::string sig = r.str(prefix + "htmlSigText");
    if (r.boolean(prefix + "htmlSigFormat", false)) sig = htmlToText(sig);
    return sig;
}

// --- Zip -------------------------------------------------------------------------

std::uint32_t crc32(const std::string& data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char b : data) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

// --- Export ----------------------------------------------------------------------

std::string escapePrefString(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\x";
                    out += hex[c >> 4];
                    out += hex[c & 0x0f];
                } else {
                    out += static_cast<char>(c);  // UTF-8 skrivs som det är, precis som Thunderbird.
                }
        }
    }
    return out;
}

class PrefWriter {
public:
    void str(const std::string& name, const std::string& value) {
        lines_.push_back("user_pref(\"" + name + "\", \"" + escapePrefString(value) + "\");");
    }
    void num(const std::string& name, long long value) {
        lines_.push_back("user_pref(\"" + name + "\", " + std::to_string(value) + ");");
    }
    void boolean(const std::string& name, bool value) {
        lines_.push_back("user_pref(\"" + name + "\", " + (value ? "true" : "false") + ");");
    }
    std::string finish() {
        // Thunderbird skriver prefs.js sorterad; gör likadant så att filen känns igen.
        std::sort(lines_.begin(), lines_.end());
        std::string out = "// Mozilla User Preferences\n\n"
                          "// Exporterad från ThunderbirdKlon. Lösenord ingår inte; Thunderbird frågar efter dem.\n\n";
        for (const auto& line : lines_) out += line + "\n";
        return out;
    }

private:
    std::vector<std::string> lines_;
};

}  // namespace

Prefs parsePrefs(const std::string& text) {
    return PrefsParser(text).parse();
}

json importPreview(const Prefs& prefs, const std::vector<Account>& existing) {
    PrefReader r(prefs);
    json accounts = json::array();
    json skipped = json::array();

    std::string defaultSmtp = r.str("mail.smtp.defaultserver");
    auto accountKeys = splitList(r.str("mail.accountmanager.accounts"));

    for (const auto& accountKey : accountKeys) {
        std::string serverKey = r.str("mail.account." + accountKey + ".server");
        if (serverKey.empty()) continue;
        std::string sp = "mail.server." + serverKey + ".";
        std::string type = r.str(sp + "type");
        std::string accountName = r.str(sp + "name", r.str(sp + "hostname"));

        if (type == "none") continue;  // Lokala mappar; inget att importera.
        if (type != "imap") {
            std::string reason = type == "pop3"  ? "POP3-konton stöds inte, bara IMAP"
                                 : type == "nntp" ? "Nyhetsgrupper stöds inte"
                                 : type == "rss"  ? "RSS-flöden stöds inte"
                                                  : "Kontotypen stöds inte";
            skipped.push_back({{"name", accountName}, {"type", type}, {"reason", reason}});
            continue;
        }

        json warnings = json::array();
        std::string host = r.str(sp + "realhostname", r.str(sp + "hostname"));
        long long socketType = r.num(sp + "socketType", kSocketPlain);
        long long imapPort = r.num(sp + "port", 0);
        std::string username = r.str(sp + "realuserName", r.str(sp + "userName"));
        std::string lowerHost = lower(host);
        bool isGoogle = lowerHost == "imap.gmail.com" || lowerHost == "imap.googlemail.com";
        bool oauth2 = r.num(sp + "authMethod", kAuthPasswordCleartext) == kAuthOAuth2;
        if (oauth2 && !isGoogle) {
            warnings.push_back("Kontot använder OAuth2 i Thunderbird, vilket bara stöds för Gmail här. Ange ett app-lösenord.");
        }

        auto identityKeys = splitList(r.str("mail.account." + accountKey + ".identities"));
        if (identityKeys.empty()) {
            skipped.push_back({{"name", accountName}, {"type", type}, {"reason", "Kontot saknar identitet"}});
            continue;
        }

        // Första identiteten blir kontots egen avsändare, övriga blir extra avsändare.
        std::string ip = "mail.identity." + identityKeys.front() + ".";
        std::string email = r.str(ip + "useremail");
        std::string smtpKey = r.str(ip + "smtpServer", defaultSmtp);
        SmtpSettings smtp = readSmtp(r, smtpKey);
        if (!smtp.found) smtp = readSmtp(r, defaultSmtp);
        if (!smtp.found) {
            std::string domain = email.find('@') != std::string::npos ? email.substr(email.find('@') + 1) : host;
            smtp.host = "smtp." + domain;
            smtp.port = 587;
            smtp.security = "starttls";
            warnings.push_back("Ingen utgående server hittades; gissade " + smtp.host + ". Kontrollera efter importen.");
        }
        if (smtp.noAuth) warnings.push_back("Den utgående servern kräver ingen inloggning i Thunderbird, men här används kontots inloggning.");
        if (r.boolean(ip + "attach_signature", false)) {
            warnings.push_back("Signaturen hämtas från en fil i Thunderbird och kunde inte importeras.");
        }

        json identities = json::array();
        for (std::size_t k = 1; k < identityKeys.size(); ++k) {
            std::string xp = "mail.identity." + identityKeys[k] + ".";
            std::string xEmail = r.str(xp + "useremail");
            if (xEmail.empty()) continue;
            SmtpSettings xs = readSmtp(r, r.str(xp + "smtpServer", defaultSmtp));
            bool sameSmtp = !xs.found || (xs.host == smtp.host && xs.port == smtp.port &&
                                          xs.security == smtp.security && xs.username == smtp.username);
            json identity = {
                {"name", r.str(xp + "fullName")},
                {"email", xEmail},
                {"replyTo", r.str(xp + "reply_to")},
                {"signature", signatureOf(r, xp)},
                {"useAccountSmtp", sameSmtp},
            };
            if (!sameSmtp) {
                identity["smtpHost"] = xs.host;
                identity["smtpPort"] = xs.port;
                identity["smtpSecurity"] = xs.security;
                identity["smtpUsername"] = xs.noAuth ? "" : xs.username;
            }
            identities.push_back(std::move(identity));
        }

        bool alreadyExists = std::any_of(existing.begin(), existing.end(), [&](const Account& a) {
            return lower(a.email) == lower(email) && lower(a.imapHost) == lower(host);
        });

        accounts.push_back({
            {"sourceKey", accountKey},
            {"accountName", accountName},
            {"name", r.str(ip + "fullName")},
            {"email", email},
            {"imapHost", host},
            {"imapPort", imapPort > 0 ? imapPort : (socketType == kSocketSSL ? 993 : 143)},
            {"imapSecurity", securityFromSocketType(socketType)},
            {"username", username},
            {"smtpHost", smtp.host},
            {"smtpPort", smtp.port},
            {"smtpSecurity", smtp.security},
            {"smtpUsername", smtp.username == username ? "" : smtp.username},
            {"signature", signatureOf(r, ip)},
            {"tlsVerify", true},
            {"identities", identities},
            {"warnings", warnings},
            {"alreadyExists", alreadyExists},
            // Gmail-konton kan logga in med Google i stället för lösenord.
            {"oauthProvider", isGoogle ? "google" : ""},
            {"oauthInThunderbird", oauth2},
        });
    }

    return {{"accounts", accounts}, {"skipped", skipped}};
}

std::string exportPrefs(const std::vector<std::pair<Account, std::vector<Identity>>>& accounts) {
    PrefWriter w;
    std::vector<std::string> accountKeys;
    std::vector<std::string> smtpKeys;
    std::map<std::string, std::string> smtpByFingerprint;  // Samma server skrivs bara en gång.

    auto smtpKeyFor = [&](const std::string& host, int port, const std::string& security,
                          const std::string& username, const std::string& description, bool oauth2) {
        std::string fingerprint =
            lower(host) + "|" + std::to_string(port) + "|" + security + "|" + username + (oauth2 ? "|oauth" : "");
        auto it = smtpByFingerprint.find(fingerprint);
        if (it != smtpByFingerprint.end()) return it->second;
        std::string key = "smtp" + std::to_string(smtpKeys.size() + 1);
        smtpKeys.push_back(key);
        smtpByFingerprint[fingerprint] = key;
        std::string p = "mail.smtpserver." + key + ".";
        w.str(p + "type", "smtp");
        w.str(p + "hostname", host);
        w.num(p + "port", port);
        w.num(p + "try_ssl", socketTypeFromSecurity(security));
        w.str(p + "username", username);
        w.num(p + "authMethod", oauth2 ? kAuthOAuth2 : username.empty() ? kAuthNone : kAuthPasswordCleartext);
        w.str(p + "description", description);
        return key;
    };

    int identityCounter = 0;
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        const auto& [account, identities] = accounts[i];
        std::string accountKey = "account" + std::to_string(i + 1);
        std::string serverKey = "server" + std::to_string(i + 1);
        accountKeys.push_back(accountKey);

        std::string sp = "mail.server." + serverKey + ".";
        w.str(sp + "type", "imap");
        w.str(sp + "hostname", account.imapHost);
        w.num(sp + "port", account.imapPort);
        w.num(sp + "socketType", socketTypeFromSecurity(account.imapSecurity));
        w.str(sp + "userName", account.username);
        w.str(sp + "name", account.email);
        w.num(sp + "authMethod", account.usesGoogleOAuth() ? kAuthOAuth2 : kAuthPasswordCleartext);
        w.boolean(sp + "login_at_startup", true);
        w.boolean(sp + "check_new_mail", true);

        std::string accountSmtpUser = account.smtpUsername.empty() ? account.username : account.smtpUsername;
        std::string accountSmtp = smtpKeyFor(account.smtpHost, account.smtpPort, account.smtpSecurity, accountSmtpUser,
                                             account.email, account.usesGoogleOAuth());

        std::vector<std::string> identityKeys;
        auto writeIdentity = [&](const std::string& fullName, const std::string& email, const std::string& replyTo,
                                 const std::string& signature, const std::string& smtpKey) {
            std::string key = "id" + std::to_string(++identityCounter);
            identityKeys.push_back(key);
            std::string ip = "mail.identity." + key + ".";
            w.str(ip + "fullName", fullName);
            w.str(ip + "useremail", email);
            if (!replyTo.empty()) w.str(ip + "reply_to", replyTo);
            w.str(ip + "smtpServer", smtpKey);
            w.boolean(ip + "valid", true);
            if (!signature.empty()) {
                w.str(ip + "htmlSigText", signature);
                w.boolean(ip + "htmlSigFormat", false);
            }
        };

        writeIdentity(account.name, account.email, "", account.signature, accountSmtp);
        for (const auto& identity : identities) {
            std::string smtpKey = identity.useAccountSmtp
                                      ? accountSmtp
                                      : smtpKeyFor(identity.smtpHost, identity.smtpPort, identity.smtpSecurity,
                                                   identity.smtpUsername, identity.email, false);
            writeIdentity(identity.name, identity.email, identity.replyTo, identity.signature, smtpKey);
        }

        std::string identityList;
        for (const auto& key : identityKeys) identityList += (identityList.empty() ? "" : ",") + key;
        w.str("mail.account." + accountKey + ".server", serverKey);
        w.str("mail.account." + accountKey + ".identities", identityList);
    }

    std::string accountList;
    for (const auto& key : accountKeys) accountList += (accountList.empty() ? "" : ",") + key;
    std::string smtpList;
    for (const auto& key : smtpKeys) smtpList += (smtpList.empty() ? "" : ",") + key;
    w.str("mail.accountmanager.accounts", accountList);
    if (!accountKeys.empty()) w.str("mail.accountmanager.defaultaccount", accountKeys.front());
    w.str("mail.smtpservers", smtpList);
    if (!smtpKeys.empty()) w.str("mail.smtp.defaultserver", smtpKeys.front());
    return w.finish();
}

std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    auto dosTime = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    auto dosDate = static_cast<std::uint16_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    constexpr std::uint16_t kUtf8Names = 0x0800;

    std::string out;
    std::string central;
    for (const auto& [name, data] : files) {
        std::uint32_t crc = crc32(data);
        auto size = static_cast<std::uint32_t>(data.size());
        auto offset = static_cast<std::uint32_t>(out.size());

        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, kUtf8Names);
        put16(out, 0);  // Lagrad, ingen komprimering.
        put16(out, dosTime);
        put16(out, dosDate);
        put32(out, crc);
        put32(out, size);
        put32(out, size);
        put16(out, static_cast<std::uint16_t>(name.size()));
        put16(out, 0);
        out += name;
        out += data;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, kUtf8Names);
        put16(central, 0);
        put16(central, dosTime);
        put16(central, dosDate);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, static_cast<std::uint16_t>(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += name;
    }

    auto centralOffset = static_cast<std::uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint16_t>(files.size()));
    put16(out, static_cast<std::uint16_t>(files.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, centralOffset);
    put16(out, 0);
    return out;
}

}  // namespace tbk::thunderbird
