#include "api.hpp"

#include "crypto.hpp"
#include "errors.hpp"
#include "thunderbird_prefs.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iostream>
#include <set>

namespace brevduva {

namespace {

using httplib::Request;
using httplib::Response;
using Handler = std::function<void(const Request&, Response&)>;
using UserHandler = std::function<void(const Request&, Response&, const User&)>;

void sendJson(Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
}

void sendError(Response& res, int status, const std::string& message) {
    sendJson(res, {{"error", message}}, status);
}

Handler guarded(Handler handler) {
    return [handler = std::move(handler)](const Request& req, Response& res) {
        try {
            handler(req, res);
        } catch (const BadRequest& e) {
            sendError(res, 400, e.what());
        } catch (const Unauthorized& e) {
            sendError(res, 401, e.what());
        } catch (const Forbidden& e) {
            sendError(res, 403, e.what());
        } catch (const NotFound& e) {
            sendError(res, 404, e.what());
        } catch (const TooManyRequests& e) {
            sendError(res, 429, e.what());
        } catch (const MailServerError& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 502, e.what());
        } catch (const NetError& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 502, e.what());
        } catch (const json::exception& e) {
            sendError(res, 400, std::string("Ogiltig förfrågan: ") + e.what());
        } catch (const std::exception& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 500, e.what());
        }
    };
}

json parseBody(const Request& req) {
    if (req.body.empty()) return json::object();
    json body = json::parse(req.body);
    if (!body.is_object()) throw BadRequest("Förväntade ett JSON-objekt");
    return body;
}

long long pathId(const Request& req) {
    try {
        return std::stoll(req.path_params.at("id"));
    } catch (...) {
        throw BadRequest("Ogiltigt id");
    }
}

std::string requiredParam(const Request& req, const std::string& name) {
    if (!req.has_param(name)) throw BadRequest("Parameter saknas: " + name);
    return req.get_param_value(name);
}

int intParam(const Request& req, const std::string& name, int fallback) {
    if (!req.has_param(name)) return fallback;
    try {
        return std::stoi(req.get_param_value(name));
    } catch (...) {
        throw BadRequest("Ogiltigt värde för " + name);
    }
}

std::uint32_t parseUid(const std::string& s) {
    try {
        long long v = std::stoll(s);
        if (v <= 0 || v > 0xffffffffLL) throw std::out_of_range("uid");
        return static_cast<std::uint32_t>(v);
    } catch (...) {
        throw BadRequest("Ogiltigt meddelande-id");
    }
}

MessageRef refFromQuery(const Request& req) {
    return {requiredParam(req, "folder"), parseUid(requiredParam(req, "uid"))};
}

std::optional<MessageRef> refFromJson(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    const json& v = j[key];
    return MessageRef{v.at("folder").get<std::string>(), v.at("uid").get<std::uint32_t>()};
}

std::vector<std::uint32_t> uidsFromJson(const json& body) {
    std::vector<std::uint32_t> uids;
    for (const auto& v : body.at("uids")) uids.push_back(v.get<std::uint32_t>());
    return uids;
}

std::string cookieValue(const Request& req, const std::string& name) {
    std::string header = req.get_header_value("Cookie");
    std::size_t pos = 0;
    while (pos < header.size()) {
        std::size_t end = header.find(';', pos);
        if (end == std::string::npos) end = header.size();
        std::string part = header.substr(pos, end - pos);
        std::size_t start = part.find_first_not_of(' ');
        if (start != std::string::npos) part = part.substr(start);
        std::size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == name) return part.substr(eq + 1);
        pos = end + 1;
    }
    return {};
}

std::string sessionToken(const Request& req) {
    return cookieValue(req, Auth::kCookieName);
}

void setSessionCookie(Response& res, const std::string& token) {
    int maxAge = token.empty() ? 0 : Auth::kSessionDays * 24 * 3600;
    res.set_header("Set-Cookie", std::string(Auth::kCookieName) + "=" + token +
                                     "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(maxAge));
}

// RFC 5987-kodning av filnamn för Content-Disposition.
std::string encodeFilename(const std::string& name) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

json userJson(const User& u) {
    return {{"id", u.id}, {"username", u.username}, {"isAdmin", u.isAdmin}, {"createdAt", u.createdAt}};
}

json identityJson(const Identity& i) {
    return {
        {"id", i.id},
        {"accountId", i.accountId},
        {"name", i.name},
        {"email", i.email},
        {"replyTo", i.replyTo},
        {"signature", i.signature},
        {"useAccountSmtp", i.useAccountSmtp},
        {"smtpHost", i.smtpHost},
        {"smtpPort", i.smtpPort},
        {"smtpSecurity", i.smtpSecurity},
        {"smtpUsername", i.smtpUsername},
        {"hasSmtpPassword", !i.smtpPassword.empty()},
        {"tlsVerify", i.tlsVerify},
    };
}

void validateSecurity(const std::string& value) {
    if (value != "none" && value != "starttls" && value != "tls") throw BadRequest("Ogiltig säkerhet: " + value);
}

// Läser identitetsfält från JSON. SMTP-lösenordet behålls om det utelämnas vid uppdatering.
Identity identityFromJson(const json& j, const std::optional<Identity>& existing) {
    Identity i = existing.value_or(Identity{});
    auto str = [&](const char* key, std::string& target) {
        if (j.contains(key)) target = j.at(key).get<std::string>();
    };
    str("name", i.name);
    str("email", i.email);
    str("replyTo", i.replyTo);
    str("signature", i.signature);
    str("smtpHost", i.smtpHost);
    str("smtpUsername", i.smtpUsername);
    if (j.contains("smtpSecurity")) {
        i.smtpSecurity = j.at("smtpSecurity").get<std::string>();
        validateSecurity(i.smtpSecurity);
    }
    if (j.contains("smtpPort")) {
        i.smtpPort = j.at("smtpPort").get<int>();
        if (i.smtpPort < 1 || i.smtpPort > 65535) throw BadRequest("Ogiltig port");
    }
    if (j.contains("useAccountSmtp")) i.useAccountSmtp = j.at("useAccountSmtp").get<bool>();
    if (j.contains("tlsVerify")) i.tlsVerify = j.at("tlsVerify").get<bool>();
    if (j.contains("smtpPassword") && !j.at("smtpPassword").get<std::string>().empty()) {
        i.smtpPassword = j.at("smtpPassword").get<std::string>();
    }
    // Utan inloggning behövs inget lösenord; rensa så att ett gammalt inte ligger kvar.
    if (i.smtpUsername.empty()) i.smtpPassword.clear();

    if (i.email.find('@') == std::string::npos) throw BadRequest("Ange en giltig e-postadress för avsändaren");
    if (!i.replyTo.empty() && i.replyTo.find('@') == std::string::npos) throw BadRequest("Ogiltig svarsadress");
    if (!i.useAccountSmtp) {
        if (i.smtpHost.empty()) throw BadRequest("SMTP-server krävs för en avsändare med egen server");
        if (!i.smtpUsername.empty() && i.smtpPassword.empty()) throw BadRequest("SMTP-lösenord krävs");
    }
    if (i.name.empty()) i.name = i.email;
    return i;
}

json accountJson(const Account& a) {
    return {
        {"id", a.id},
        {"name", a.name},
        {"email", a.email},
        {"imapHost", a.imapHost},
        {"imapPort", a.imapPort},
        {"imapSecurity", a.imapSecurity},
        {"smtpHost", a.smtpHost},
        {"smtpPort", a.smtpPort},
        {"smtpSecurity", a.smtpSecurity},
        {"username", a.username},
        {"smtpUsername", a.smtpUsername},
        {"tlsVerify", a.tlsVerify},
        {"signature", a.signature},
        {"authType", a.authType},
    };
}

// Läser kontofält från JSON. Lösenordet får utelämnas vid uppdatering. Med authType "oauth-google" och
// oauthState hämtas användarnamn och token från en avslutad Google-inloggning (utan att förbruka den).
Account accountFromJson(const json& j, const std::optional<Account>& existing, GoogleOAuth& oauth, long long userId) {
    Account a = existing.value_or(Account{});
    auto str = [&](const char* key, std::string& target) {
        if (j.contains(key)) target = j.at(key).get<std::string>();
    };
    auto port = [&](const char* key, int& target) {
        if (!j.contains(key)) return;
        int v = j.at(key).get<int>();
        if (v < 1 || v > 65535) throw BadRequest(std::string("Ogiltig port: ") + key);
        target = v;
    };
    auto security = [&](const char* key, std::string& target) {
        if (!j.contains(key)) return;
        std::string v = j.at(key).get<std::string>();
        if (v != "none" && v != "starttls" && v != "tls") throw BadRequest(std::string("Ogiltig säkerhet: ") + key);
        target = v;
    };

    str("name", a.name);
    str("email", a.email);
    str("imapHost", a.imapHost);
    port("imapPort", a.imapPort);
    security("imapSecurity", a.imapSecurity);
    str("smtpHost", a.smtpHost);
    port("smtpPort", a.smtpPort);
    security("smtpSecurity", a.smtpSecurity);
    str("username", a.username);
    str("smtpUsername", a.smtpUsername);
    str("signature", a.signature);
    if (j.contains("tlsVerify")) a.tlsVerify = j.at("tlsVerify").get<bool>();
    if (j.contains("password") && !j.at("password").get<std::string>().empty()) {
        a.password = j.at("password").get<std::string>();
    }

    if (j.contains("authType")) {
        std::string type = j.at("authType").get<std::string>();
        if (type != "password" && type != "oauth-google") throw BadRequest("Ogiltig inloggningstyp");
        a.authType = type;
    }
    if (a.usesGoogleOAuth()) {
        std::string state = j.value("oauthState", "");
        if (!state.empty()) {
            auto result = oauth.status(state, userId);
            if (!result || !result->done || !result->error.empty()) {
                throw BadRequest("Google-inloggningen är inte klar eller har gått ut. Logga in med Google igen.");
            }
            a.oauthRefreshToken = result->refreshToken;
            a.oauthAccessToken = result->accessToken;
            a.username = result->email;
            if (a.email.empty()) a.email = result->email;
            if (a.name.empty()) a.name = result->name;
        }
        if (a.oauthRefreshToken.empty()) throw BadRequest("Logga in med Google för att använda Google-inloggning");
        a.password.clear();
    } else {
        a.oauthRefreshToken.clear();
        a.oauthAccessToken.clear();
    }

    if (a.email.find('@') == std::string::npos) throw BadRequest("Ange en giltig e-postadress");
    if (a.imapHost.empty() || a.smtpHost.empty()) throw BadRequest("Server för IMAP och SMTP krävs");
    if (a.username.empty()) throw BadRequest("Användarnamn krävs");
    if (!a.usesGoogleOAuth() && a.password.empty()) throw BadRequest("Lösenord krävs");
    if (a.name.empty()) a.name = a.email;
    return a;
}

// Tolkar multipart-förfrågan från skrivfönstret: fältet "message" (JSON) och filer i "attachments".
std::pair<mime::OutgoingMessage, SendOptions> composeFromRequest(const Request& req) {
    json j;
    if (req.is_multipart_form_data()) {
        if (!req.has_file("message")) throw BadRequest("Fältet message saknas");
        j = json::parse(req.get_file_value("message").content);
    } else {
        j = parseBody(req);
    }

    mime::OutgoingMessage msg;
    msg.to = j.value("to", "");
    msg.cc = j.value("cc", "");
    msg.bcc = j.value("bcc", "");
    msg.subject = j.value("subject", "");
    msg.text = j.value("text", "");
    msg.inReplyTo = j.value("inReplyTo", "");
    msg.references = j.value("references", "");

    if (req.is_multipart_form_data()) {
        for (const auto& file : req.get_file_values("attachments")) {
            msg.attachments.push_back({file.filename, file.content_type, file.content});
        }
    }

    SendOptions opts;
    opts.replyTo = refFromJson(j, "replyTo");
    opts.forwardOf = refFromJson(j, "forwardOf");
    opts.draft = refFromJson(j, "draft");
    if (j.contains("identityId") && !j["identityId"].is_null()) opts.identityId = j["identityId"].get<long long>();
    return {std::move(msg), opts};
}

// Ett e-postkonto som tillhör den inloggade användaren. Andras konton ger 404, så att id:n inte avslöjas.
Account ownedAccount(Database& db, long long accountId, const User& user) {
    auto account = db.getAccount(accountId);
    if (!account || account->userId != user.id) throw NotFound("Kontot finns inte");
    return *account;
}

long long ownedAccountId(Database& db, const Request& req, const User& user) {
    return ownedAccount(db, pathId(req), user).id;
}

Identity ownedIdentity(Database& db, const Request& req, const Account& account) {
    long long id;
    try {
        id = std::stoll(req.path_params.at("identityId"));
    } catch (...) {
        throw BadRequest("Ogiltigt avsändar-id");
    }
    auto identity = db.getIdentity(id);
    if (!identity || identity->accountId != account.id) throw NotFound("Avsändaren finns inte");
    return *identity;
}

void requireConnectionTest(MailService& mail, Response& res, const json& body, const Account& a, bool& aborted) {
    aborted = false;
    if (body.value("skipTest", false)) return;
    json test = mail.testAccount(a);
    if (!test["imap"]["ok"].get<bool>() || !test["smtp"]["ok"].get<bool>()) {
        sendJson(res, {{"error", "Anslutningstestet misslyckades"}, {"test", test}}, 400);
        aborted = true;
    }
}

// Sidan som Google omdirigerar till. Den meddelar appens flik via BroadcastChannel och stänger sig;
// appen hämtar sedan resultatet via /api/oauth/google/status med sin session.
std::string oauthCallbackPage(const std::string& state, bool ok) {
    std::string message = ok ? "Inloggningen är klar. Du kan stänga det här fönstret."
                             : "Inloggningen misslyckades. Stäng fönstret och försök igen.";
    return "<!doctype html><html lang=\"sv\"><head><meta charset=\"utf-8\"><title>Google-inloggning</title>"
           "<style>body{font:15px system-ui,sans-serif;margin:48px;text-align:center;color:#15141a}</style></head>"
           "<body><p>" + message + "</p><script>"
           "try{new BroadcastChannel('brevduva-oauth').postMessage({state:'" + state + "'})}catch(e){}"
           "setTimeout(function(){window.close()},400);</script></body></html>";
}

bool validState(const std::string& state) {
    return !state.empty() && state.size() <= 128 &&
           std::all_of(state.begin(), state.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// Återkallar en refresh-token hos Google om inget annat av användarens konton använder samma Google-konto.
void revokeIfUnused(Database& db, GoogleOAuth& oauth, const Account& removed) {
    if (!removed.usesGoogleOAuth() || removed.oauthRefreshToken.empty()) return;
    for (const auto& other : db.listAccounts(removed.userId)) {
        if (other.id != removed.id && other.usesGoogleOAuth() && other.username == removed.username) return;
    }
    oauth.revoke(removed.oauthRefreshToken);
}

}  // namespace

void registerApi(httplib::Server& server, Database& db, Auth& auth, MailService& mail, GoogleOAuth& oauth) {
    // CSRF-skydd: ändrande anrop måste bära en egen header, som webbläsare inte skickar cross-site utan CORS.
    server.set_pre_routing_handler([](const Request& req, Response& res) {
        if (req.path.rfind("/api/", 0) != 0) return httplib::Server::HandlerResponse::Unhandled;
        if (req.method != "GET" && req.method != "HEAD" && req.get_header_value("X-Brevduva") != "1") {
            sendError(res, 403, "Förfrågan saknar X-Brevduva-header");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Omsluter en handler som kräver inloggning (och eventuellt administratör).
    auto authed = [&auth](UserHandler handler, bool adminOnly = false) {
        return guarded([&auth, handler = std::move(handler), adminOnly](const Request& req, Response& res) {
            auto user = auth.userForToken(sessionToken(req));
            if (!user) throw Unauthorized("Inte inloggad");
            if (adminOnly && !user->isAdmin) throw Forbidden("Kräver administratörsbehörighet");
            handler(req, res, *user);
        });
    };

    // --- Session och installation -----------------------------------------
    server.Get("/api/session", guarded([&auth](const Request& req, Response& res) {
        auto user = auth.userForToken(sessionToken(req));
        sendJson(res, {
                          {"setupRequired", auth.setupRequired()},
                          {"authenticated", user.has_value()},
                          {"user", user ? userJson(*user) : json(nullptr)},
                      });
    }));

    server.Post("/api/setup", guarded([&db, &auth](const Request& req, Response& res) {
        json body = parseBody(req);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        Auth::validateUsername(username);
        Auth::validatePassword(password);
        auto id = db.createFirstAdmin(username, crypto::hashPassword(password));
        if (!id) throw Forbidden("Installationen är redan gjord");
        std::cout << "[auth] administratören " << username << " skapades via installationssidan" << std::endl;
        setSessionCookie(res, auth.createSession(*id));
        sendJson(res, {{"authenticated", true}, {"user", userJson(*db.getUser(*id))}}, 201);
    }));

    server.Post("/api/login", guarded([&auth](const Request& req, Response& res) {
        json body = parseBody(req);
        auto user = auth.login(body.value("username", ""), body.value("password", ""), req.remote_addr);
        if (!user) throw Unauthorized("Fel användarnamn eller lösenord");
        setSessionCookie(res, auth.createSession(user->id));
        sendJson(res, {{"authenticated", true}, {"user", userJson(*user)}});
    }));

    server.Post("/api/logout", guarded([&auth](const Request& req, Response& res) {
        auth.destroySession(sessionToken(req));
        setSessionCookie(res, "");
        sendJson(res, {{"ok", true}});
    }));

    server.Post("/api/me/password", authed([&db, &auth](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        if (!auth.checkPassword(user.id, body.value("currentPassword", ""))) {
            throw BadRequest("Nuvarande lösenord stämmer inte");
        }
        std::string next = body.value("newPassword", "");
        Auth::validatePassword(next);
        db.setUserPassword(user.id, crypto::hashPassword(next));
        auth.destroyOtherSessions(user.id, sessionToken(req));
        sendJson(res, {{"ok", true}});
    }));

    // --- Användare (administratör) -----------------------------------------
    server.Get("/api/users", authed([&db](const Request&, Response& res, const User&) {
        json out = json::array();
        for (const auto& u : db.listUsers()) out.push_back(userJson(u));
        sendJson(res, out);
    }, true));

    server.Post("/api/users", authed([&db](const Request& req, Response& res, const User& admin) {
        json body = parseBody(req);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        Auth::validateUsername(username);
        Auth::validatePassword(password);
        long long id = db.createUser(username, crypto::hashPassword(password), body.value("isAdmin", false));
        std::cout << "[auth] " << admin.username << " skapade användaren " << username << std::endl;
        sendJson(res, userJson(*db.getUser(id)), 201);
    }, true));

    server.Put("/api/users/:id", authed([&db, &auth](const Request& req, Response& res, const User& admin) {
        long long id = pathId(req);
        auto target = db.getUser(id);
        if (!target) throw NotFound("Användaren finns inte");
        json body = parseBody(req);

        if (body.contains("isAdmin")) {
            bool makeAdmin = body.at("isAdmin").get<bool>();
            if (!makeAdmin && target->isAdmin) {
                if (id == admin.id) throw BadRequest("Du kan inte ta bort din egen administratörsbehörighet");
                if (db.countAdmins() <= 1) throw BadRequest("Det måste finnas minst en administratör");
            }
            db.setUserAdmin(id, makeAdmin);
        }
        if (body.contains("password")) {
            std::string password = body.at("password").get<std::string>();
            Auth::validatePassword(password);
            db.setUserPassword(id, crypto::hashPassword(password));
            // Ett återställt lösenord loggar ut användaren överallt (utom administratören själv i denna flik).
            auth.destroyOtherSessions(id, id == admin.id ? sessionToken(req) : "");
        }
        sendJson(res, userJson(*db.getUser(id)));
    }, true));

    server.Delete("/api/users/:id", authed([&db, &mail](const Request& req, Response& res, const User& admin) {
        long long id = pathId(req);
        if (id == admin.id) throw BadRequest("Du kan inte ta bort dig själv");
        if (!db.getUser(id)) throw NotFound("Användaren finns inte");
        for (const auto& account : db.listAccounts(id)) mail.dropSession(account.id);
        db.deleteUser(id);
        sendJson(res, {{"ok", true}});
    }, true));

    // --- E-postkonton -------------------------------------------------------
    server.Get("/api/accounts", authed([&db](const Request&, Response& res, const User& user) {
        json out = json::array();
        for (const auto& a : db.listAccounts(user.id)) {
            json item = accountJson(a);
            item["identities"] = json::array();
            for (const auto& i : db.listIdentities(a.id)) item["identities"].push_back(identityJson(i));
            out.push_back(std::move(item));
        }
        sendJson(res, out);
    }));

    server.Post("/api/accounts/test", authed([&db, &mail, &oauth](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        std::optional<Account> existing;
        if (body.contains("id") && !body["id"].is_null()) {
            existing = ownedAccount(db, body["id"].get<long long>(), user);
        }
        sendJson(res, mail.testAccount(accountFromJson(body, existing, oauth, user.id)));
    }));

    server.Post("/api/accounts", authed([&db, &mail, &oauth](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        Account a = accountFromJson(body, std::nullopt, oauth, user.id);
        a.userId = user.id;
        bool aborted = false;
        requireConnectionTest(mail, res, body, a, aborted);
        if (aborted) return;
        a.id = db.createAccount(a);
        if (body.contains("oauthState")) oauth.take(body.value("oauthState", ""), user.id);
        sendJson(res, accountJson(a), 201);
    }));

    server.Put("/api/accounts/:id", authed([&db, &mail, &oauth](const Request& req, Response& res, const User& user) {
        Account existing = ownedAccount(db, pathId(req), user);
        json body = parseBody(req);
        Account a = accountFromJson(body, existing, oauth, user.id);
        a.id = existing.id;
        a.userId = existing.userId;
        bool aborted = false;
        requireConnectionTest(mail, res, body, a, aborted);
        if (aborted) return;
        db.updateAccount(a);
        if (body.contains("oauthState")) oauth.take(body.value("oauthState", ""), user.id);
        // Byte från Google-inloggning till lösenord: återkalla den gamla behörigheten. En ny Google-inloggning
        // återkallar inte, eftersom Google då även kan ogiltigförklara den nya token.
        if (existing.usesGoogleOAuth() && !a.usesGoogleOAuth()) revokeIfUnused(db, oauth, existing);
        oauth.forget(a.id);
        mail.dropSession(a.id);
        sendJson(res, accountJson(a));
    }));

    server.Delete("/api/accounts/:id", authed([&db, &mail, &oauth](const Request& req, Response& res, const User& user) {
        Account account = ownedAccount(db, pathId(req), user);
        mail.dropSession(account.id);
        revokeIfUnused(db, oauth, account);
        oauth.forget(account.id);
        db.deleteAccount(account.id);
        sendJson(res, {{"ok", true}});
    }));

    // --- Google-inloggning (OAuth 2.0) ----------------------------------------
    server.Get("/api/oauth/providers", authed([&oauth](const Request& req, Response& res, const User&) {
        sendJson(res, {{"google", {{"configured", oauth.configured()},
                                   {"redirectUri", oauth.redirectUri(req.get_header_value("Host"))}}}});
    }));

    server.Post("/api/oauth/google/start", authed([&oauth](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        auto [state, url] =
            oauth.start(user.id, body.value("loginHint", ""), oauth.redirectUri(req.get_header_value("Host")));
        sendJson(res, {{"state", state}, {"url", url}});
    }));

    // Anropas av Google i webbläsaren. Sessionscookien skickas inte (SameSite=Strict), så inloggningen
    // knyts till användaren via det slumpade state-värdet.
    server.Get("/api/oauth/google/callback", guarded([&oauth](const Request& req, Response& res) {
        std::string state = req.get_param_value("state");
        bool known = validState(state) &&
                     oauth.handleCallback(state, req.get_param_value("code"), req.get_param_value("error"));
        res.set_header("Cache-Control", "no-store");
        res.set_content(oauthCallbackPage(known ? state : "", known && req.get_param_value("error").empty()),
                        "text/html; charset=utf-8");
    }));

    server.Get("/api/oauth/google/status", authed([&oauth](const Request& req, Response& res, const User& user) {
        auto result = oauth.status(req.get_param_value("state"), user.id);
        if (!result) throw NotFound("Inloggningen finns inte eller har gått ut");
        sendJson(res, {{"done", result->done},
                       {"ok", result->done && result->error.empty()},
                       {"error", result->error},
                       {"email", result->email},
                       {"name", result->name}});
    }));

    // --- Import och export i Thunderbirds format -----------------------------
    server.Post("/api/import/thunderbird/preview",
                authed([&db](const Request& req, Response& res, const User& user) {
                    json body = parseBody(req);
                    std::string prefs = body.value("prefs", "");
                    if (prefs.find("user_pref") == std::string::npos) {
                        throw BadRequest("Filen innehåller inga Thunderbird-inställningar (prefs.js)");
                    }
                    auto preview = thunderbird::importPreview(thunderbird::parsePrefs(prefs), db.listAccounts(user.id));
                    sendJson(res, preview);
                }));

    server.Get("/api/export/thunderbird", authed([&db](const Request&, Response& res, const User& user) {
        std::vector<std::pair<Account, std::vector<Identity>>> data;
        for (auto& account : db.listAccounts(user.id)) {
            auto identities = db.listIdentities(account.id);
            data.emplace_back(std::move(account), std::move(identities));
        }
        std::string zip = thunderbird::makeZip({{"prefs.js", thunderbird::exportPrefs(data)}});

        char date[16];
        std::time_t now = std::time(nullptr);
        std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
        res.set_header("Content-Disposition",
                       std::string("attachment; filename=\"brevduva-installningar-") + date + ".zip\"");
        res.set_header("Cache-Control", "no-store");
        res.set_content(zip, "application/zip");
    }));

    // --- Avsändaridentiteter ------------------------------------------------
    server.Post("/api/accounts/:id/identities/test",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    Account account = ownedAccount(db, pathId(req), user);
                    json body = parseBody(req);
                    std::optional<Identity> existing;
                    if (body.contains("id") && !body["id"].is_null()) {
                        auto found = db.getIdentity(body["id"].get<long long>());
                        if (!found || found->accountId != account.id) throw NotFound("Avsändaren finns inte");
                        existing = found;
                    }
                    sendJson(res, mail.testIdentitySmtp(account, identityFromJson(body, existing)));
                }));

    auto saveIdentity = [&db, &mail](Response& res, const json& body, const Account& account, Identity identity,
                              bool created) {
        identity.accountId = account.id;
        if (!identity.useAccountSmtp && !body.value("skipTest", false)) {
            json test = mail.testIdentitySmtp(account, identity);
            if (!test["ok"].get<bool>()) {
                sendJson(res, {{"error", "SMTP-testet misslyckades"}, {"test", test}}, 400);
                return;
            }
        }
        if (created) {
            identity.id = db.createIdentity(identity);
        } else {
            db.updateIdentity(identity);
        }
        sendJson(res, identityJson(identity), created ? 201 : 200);
    };

    server.Post("/api/accounts/:id/identities",
                authed([&db, saveIdentity](const Request& req, Response& res, const User& user) {
                    Account account = ownedAccount(db, pathId(req), user);
                    json body = parseBody(req);
                    saveIdentity(res, body, account, identityFromJson(body, std::nullopt), true);
                }));

    server.Put("/api/accounts/:id/identities/:identityId",
               authed([&db, saveIdentity](const Request& req, Response& res, const User& user) {
                   Account account = ownedAccount(db, pathId(req), user);
                   Identity existing = ownedIdentity(db, req, account);
                   json body = parseBody(req);
                   Identity identity = identityFromJson(body, existing);
                   identity.id = existing.id;
                   saveIdentity(res, body, account, identity, false);
               }));

    server.Delete("/api/accounts/:id/identities/:identityId",
                  authed([&db](const Request& req, Response& res, const User& user) {
                      Account account = ownedAccount(db, pathId(req), user);
                      db.deleteIdentity(ownedIdentity(db, req, account).id);
                      sendJson(res, {{"ok", true}});
                  }));

    // --- Mappar ------------------------------------------------------------
    server.Get("/api/accounts/:id/folders", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        sendJson(res, mail.listFolders(ownedAccountId(db, req, user)));
    }));

    server.Post("/api/accounts/:id/folders", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        mail.createFolder(ownedAccountId(db, req, user), parseBody(req).at("path").get<std::string>());
        sendJson(res, {{"ok", true}});
    }));

    server.Post("/api/accounts/:id/folders/rename",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    json body = parseBody(req);
                    mail.renameFolder(ownedAccountId(db, req, user), body.at("from").get<std::string>(),
                                      body.at("to").get<std::string>());
                    sendJson(res, {{"ok", true}});
                }));

    server.Post("/api/accounts/:id/folders/delete",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    mail.deleteFolder(ownedAccountId(db, req, user), parseBody(req).at("path").get<std::string>());
                    sendJson(res, {{"ok", true}});
                }));

    // --- Meddelanden -------------------------------------------------------
    server.Get("/api/accounts/:id/messages", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        MessageQuery q;
        q.folder = requiredParam(req, "folder");
        q.page = std::max(0, intParam(req, "page", 0));
        q.pageSize = std::clamp(intParam(req, "pageSize", 50), 1, 500);
        q.text = req.has_param("q") ? req.get_param_value("q") : "";
        q.unreadOnly = req.get_param_value("unread") == "1";
        q.flaggedOnly = req.get_param_value("flagged") == "1";
        sendJson(res, mail.listMessages(ownedAccountId(db, req, user), q));
    }));

    server.Get("/api/accounts/:id/message", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        bool markSeen = req.get_param_value("markSeen") != "0";
        sendJson(res, mail.getMessage(ownedAccountId(db, req, user), refFromQuery(req), markSeen));
    }));

    server.Get("/api/accounts/:id/attachment",
               authed([&db, &mail](const Request& req, Response& res, const User& user) {
                   auto att = mail.getAttachment(ownedAccountId(db, req, user), refFromQuery(req),
                                                 intParam(req, "index", -1));
                   // Bara rasterbilder får visas direkt; allt annat laddas ned för att undvika XSS via bilagor.
                   static const std::set<std::string> inlineSafe = {"image/png", "image/jpeg", "image/gif",
                                                                    "image/webp"};
                   bool inlineView = req.get_param_value("download") != "1" && inlineSafe.count(att.contentType);
                   std::string type = inlineView ? att.contentType : "application/octet-stream";
                   res.set_header("Content-Disposition", std::string(inlineView ? "inline" : "attachment") +
                                                             "; filename*=UTF-8''" + encodeFilename(att.filename));
                   res.set_header("Content-Security-Policy", "default-src 'none'; sandbox");
                   res.set_header("Cache-Control", "private, max-age=3600");
                   res.set_content(att.data, type);
               }));

    server.Get("/api/accounts/:id/source", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        std::string raw = mail.getSource(ownedAccountId(db, req, user), refFromQuery(req));
        res.set_header("Content-Security-Policy", "default-src 'none'; sandbox");
        res.set_content(raw, "text/plain; charset=utf-8");
    }));

    server.Post("/api/accounts/:id/messages/flags",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    json body = parseBody(req);
                    mail.setFlags(ownedAccountId(db, req, user), body.at("folder").get<std::string>(),
                                  uidsFromJson(body), body.value("add", std::vector<std::string>{}),
                                  body.value("remove", std::vector<std::string>{}));
                    sendJson(res, {{"ok", true}});
                }));

    server.Post("/api/accounts/:id/messages/move",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    json body = parseBody(req);
                    mail.moveMessages(ownedAccountId(db, req, user), body.at("folder").get<std::string>(),
                                      uidsFromJson(body), body.at("target").get<std::string>());
                    sendJson(res, {{"ok", true}});
                }));

    server.Post("/api/accounts/:id/messages/delete",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    json body = parseBody(req);
                    mail.deleteMessages(ownedAccountId(db, req, user), body.at("folder").get<std::string>(),
                                        uidsFromJson(body));
                    sendJson(res, {{"ok", true}});
                }));

    server.Post("/api/accounts/:id/send", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        long long id = ownedAccountId(db, req, user);
        auto [msg, opts] = composeFromRequest(req);
        sendJson(res, mail.send(id, std::move(msg), opts));
    }));

    server.Post("/api/accounts/:id/drafts", authed([&db, &mail](const Request& req, Response& res, const User& user) {
        long long id = ownedAccountId(db, req, user);
        auto [msg, opts] = composeFromRequest(req);
        sendJson(res, mail.saveDraft(id, std::move(msg), opts));
    }));
}

}  // namespace brevduva
