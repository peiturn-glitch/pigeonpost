#include "api.hpp"

#include "crypto.hpp"
#include "errors.hpp"
#include "text_util.hpp"
#include "thunderbird_prefs.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <memory>
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
    // Utan charset-parameter: JSON är alltid UTF-8, och httplib gzippar bara svar vars typ är exakt application/json.
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

void sendError(Response& res, int status, const std::string& message) {
    sendJson(res, {{"error", message}}, status);
}

// Anrop som tar längre än så här loggas med tidsåtgång, så att det går att se i efterhand
// vad som var långsamt (t.ex. en trög mejlserver) när proxyn framför har gett upp.
constexpr double kSlowRequestSeconds = 5.0;

Handler guarded(Handler handler) {
    return [handler = std::move(handler)](const Request& req, Response& res) {
        auto start = std::chrono::steady_clock::now();
        struct SlowLog {
            const Request& req;
            const Response& res;
            std::chrono::steady_clock::time_point start;
            ~SlowLog() {
                double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (seconds > kSlowRequestSeconds) {
                    std::cerr << "[api] slow request: " << req.method << " " << req.path << " took "
                              << std::round(seconds * 10) / 10 << " s (status " << res.status << ")\n";
                }
            }
        } slowLog{req, res, start};
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
        } catch (const Gone& e) {
            sendError(res, 410, e.what());
        } catch (const MailServerError& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 502, e.what());
        } catch (const NetError& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 502, e.what());
        } catch (const AiError& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 502, e.what());
        } catch (const json::exception& e) {
            sendError(res, 400, std::string("Invalid request: ") + e.what());
        } catch (const std::exception& e) {
            std::cerr << "[api] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 500, e.what());
        }
    };
}

json parseBody(const Request& req) {
    if (req.body.empty()) return json::object();
    json body = json::parse(req.body);
    if (!body.is_object()) throw BadRequest("Expected a JSON object");
    return body;
}

long long pathId(const Request& req) {
    try {
        return std::stoll(req.path_params.at("id"));
    } catch (...) {
        throw BadRequest("Invalid id");
    }
}

std::string requiredParam(const Request& req, const std::string& name) {
    if (!req.has_param(name)) throw BadRequest("Missing parameter: " + name);
    return req.get_param_value(name);
}

int intParam(const Request& req, const std::string& name, int fallback) {
    if (!req.has_param(name)) return fallback;
    try {
        return std::stoi(req.get_param_value(name));
    } catch (...) {
        throw BadRequest("Invalid value for " + name);
    }
}

std::uint32_t parseUid(const std::string& s) {
    try {
        long long v = std::stoll(s);
        if (v <= 0 || v > 0xffffffffLL) throw std::out_of_range("uid");
        return static_cast<std::uint32_t>(v);
    } catch (...) {
        throw BadRequest("Invalid message id");
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
        {"signatureHtml", i.signatureHtml},
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
    if (value != "none" && value != "starttls" && value != "tls") throw BadRequest("Invalid security: " + value);
}

// En formaterad signatur kan innehålla en logotyp som data-URI, men ska inte kunna fylla databasen.
void validateSignature(const std::string& text, const std::string& html) {
    constexpr std::size_t kMaxSignature = 2 * 1024 * 1024;
    if (text.size() > kMaxSignature || html.size() > kMaxSignature) {
        throw BadRequest("The signature is too large (max 2 MB). Use a smaller image.");
    }
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
    str("signatureHtml", i.signatureHtml);
    validateSignature(i.signature, i.signatureHtml);
    str("smtpHost", i.smtpHost);
    str("smtpUsername", i.smtpUsername);
    if (j.contains("smtpSecurity")) {
        i.smtpSecurity = j.at("smtpSecurity").get<std::string>();
        validateSecurity(i.smtpSecurity);
    }
    if (j.contains("smtpPort")) {
        i.smtpPort = j.at("smtpPort").get<int>();
        if (i.smtpPort < 1 || i.smtpPort > 65535) throw BadRequest("Invalid port");
    }
    if (j.contains("useAccountSmtp")) i.useAccountSmtp = j.at("useAccountSmtp").get<bool>();
    if (j.contains("tlsVerify")) i.tlsVerify = j.at("tlsVerify").get<bool>();
    if (j.contains("smtpPassword") && !j.at("smtpPassword").get<std::string>().empty()) {
        i.smtpPassword = j.at("smtpPassword").get<std::string>();
    }
    // Utan inloggning behövs inget lösenord; rensa så att ett gammalt inte ligger kvar.
    if (i.smtpUsername.empty()) i.smtpPassword.clear();

    if (i.email.find('@') == std::string::npos) throw BadRequest("Enter a valid email address for the sender");
    if (!i.replyTo.empty() && i.replyTo.find('@') == std::string::npos) throw BadRequest("Invalid reply-to address");
    if (!i.useAccountSmtp) {
        if (i.smtpHost.empty()) throw BadRequest("An SMTP server is required for a sender with its own server");
        if (!i.smtpUsername.empty() && i.smtpPassword.empty()) throw BadRequest("SMTP password is required");
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
        {"signatureHtml", a.signatureHtml},
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
        if (v < 1 || v > 65535) throw BadRequest(std::string("Invalid port: ") + key);
        target = v;
    };
    auto security = [&](const char* key, std::string& target) {
        if (!j.contains(key)) return;
        std::string v = j.at(key).get<std::string>();
        if (v != "none" && v != "starttls" && v != "tls") throw BadRequest(std::string("Invalid security: ") + key);
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
    str("signatureHtml", a.signatureHtml);
    validateSignature(a.signature, a.signatureHtml);
    if (j.contains("tlsVerify")) a.tlsVerify = j.at("tlsVerify").get<bool>();
    if (j.contains("password") && !j.at("password").get<std::string>().empty()) {
        a.password = j.at("password").get<std::string>();
    }

    if (j.contains("authType")) {
        std::string type = j.at("authType").get<std::string>();
        if (type != "password" && type != "oauth-google") throw BadRequest("Invalid login type");
        a.authType = type;
    }
    if (a.usesGoogleOAuth()) {
        std::string state = j.value("oauthState", "");
        if (!state.empty()) {
            auto result = oauth.status(state, userId);
            if (!result || !result->done || !result->error.empty() || result->purpose != GoogleOAuth::Purpose::Mail) {
                throw BadRequest("The Google sign-in is not complete or has expired. Sign in with Google again.");
            }
            a.oauthRefreshToken = result->refreshToken;
            a.oauthAccessToken = result->accessToken;
            a.username = result->email;
            if (a.email.empty()) a.email = result->email;
            if (a.name.empty()) a.name = result->name;
        }
        if (a.oauthRefreshToken.empty()) throw BadRequest("Sign in with Google to use Google sign-in");
        a.password.clear();
    } else {
        a.oauthRefreshToken.clear();
        a.oauthAccessToken.clear();
    }

    if (a.email.find('@') == std::string::npos) throw BadRequest("Enter a valid email address");
    if (a.imapHost.empty() || a.smtpHost.empty()) throw BadRequest("IMAP and SMTP servers are required");
    if (a.username.empty()) throw BadRequest("Username is required");
    if (!a.usesGoogleOAuth() && a.password.empty()) throw BadRequest("Password is required");
    if (a.name.empty()) a.name = a.email;
    return a;
}

// Tolkar multipart-förfrågan från skrivfönstret: fältet "message" (JSON) och filer i "attachments".
std::pair<mime::OutgoingMessage, SendOptions> composeFromRequest(const Request& req) {
    json j;
    if (req.is_multipart_form_data()) {
        if (!req.has_file("message")) throw BadRequest("The message field is missing");
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
    msg.html = j.value("html", "");
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
    if (!account || account->userId != user.id) throw NotFound("The account does not exist");
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
        throw BadRequest("Invalid sender id");
    }
    auto identity = db.getIdentity(id);
    if (!identity || identity->accountId != account.id) throw NotFound("The sender does not exist");
    return *identity;
}

void requireConnectionTest(MailService& mail, Response& res, const json& body, const Account& a, bool& aborted) {
    aborted = false;
    if (body.value("skipTest", false)) return;
    json test = mail.testAccount(a);
    if (!test["imap"]["ok"].get<bool>() || !test["smtp"]["ok"].get<bool>()) {
        sendJson(res, {{"error", "The connection test failed"}, {"test", test}}, 400);
        aborted = true;
    }
}

// Sidan som Google omdirigerar till. Den meddelar appens flik via BroadcastChannel och stänger sig;
// appen hämtar sedan resultatet via /api/oauth/google/status med sin session.
std::string oauthCallbackPage(const std::string& state, bool ok) {
    std::string message = ok ? "Sign-in complete. You can close this window."
                             : "Sign-in failed. Close this window and try again.";
    return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Google sign-in</title>"
           "<style>body{font:15px system-ui,sans-serif;margin:48px;text-align:center;color:#15141a}</style></head>"
           "<body><p>" + message + "</p><script>"
           "try{new BroadcastChannel('brevduva-oauth').postMessage({state:'" + state + "'})}catch(e){}"
           "setTimeout(function(){window.close()},400);</script></body></html>";
}

bool validState(const std::string& state) {
    return !state.empty() && state.size() <= 128 &&
           std::all_of(state.begin(), state.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// Google återkallar hela appens behörighet för ett Google-konto när en token återkallas. Därför återkallas den
// bara om inget annat av användarens e-postkonton eller Drive-kopplingar använder samma Google-konto.
bool googleAccountInUse(Database& db, long long userId, const std::string& email, long long exceptAccountId,
                        long long exceptDriveId) {
    for (const auto& other : db.listAccounts(userId)) {
        if (other.id != exceptAccountId && other.usesGoogleOAuth() && other.username == email) return true;
    }
    for (const auto& other : db.listDriveConnections(userId)) {
        if (other.id != exceptDriveId && other.email == email) return true;
    }
    return false;
}

void revokeIfUnused(Database& db, GoogleOAuth& oauth, const Account& removed) {
    if (!removed.usesGoogleOAuth() || removed.oauthRefreshToken.empty()) return;
    if (googleAccountInUse(db, removed.userId, removed.username, removed.id, 0)) return;
    oauth.revoke(removed.oauthRefreshToken);
}

json driveConnectionJson(const DriveConnection& c) {
    return {{"id", c.id}, {"email", c.email}, {"createdAt", c.createdAt}};
}

json parseStored(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

json backupRunJson(const BackupRun& r) {
    return {
        {"id", r.id},
        {"jobId", r.jobId ? json(r.jobId) : json(nullptr)},
        {"status", r.status},
        {"config", parseStored(r.config)},
        {"progress", parseStored(r.progress)},
        {"message", r.message},
        {"fileSize", r.fileSize},
        {"hasFile", !r.filePath.empty()},
        {"createdAt", r.createdAt},
        {"startedAt", r.startedAt},
        {"finishedAt", r.finishedAt},
        {"expiresAt", r.expiresAt},
    };
}

json backupJobJson(Database& db, const BackupJob& j) {
    auto last = db.lastBackupRun(j.id);
    return {
        {"id", j.id},
        {"name", j.name},
        {"config", parseStored(j.config)},
        {"schedule", parseStored(j.schedule)},
        {"enabled", j.enabled},
        {"nextRunAt", j.nextRunAt},
        {"lastRun", last ? backupRunJson(*last) : json(nullptr)},
    };
}

BackupJob ownedBackupJob(Database& db, const Request& req, const User& user) {
    auto job = db.getBackupJob(pathId(req));
    if (!job || job->userId != user.id) throw NotFound("The backup does not exist");
    return *job;
}

BackupRun ownedBackupRun(Database& db, const Request& req, const User& user) {
    auto run = db.getBackupRun(pathId(req));
    if (!run || run->userId != user.id) throw NotFound("The run does not exist");
    return *run;
}

// Läser namn, inställningar och schema för en sparad backup och räknar ut nästa körning.
void backupJobFromJson(BackupService& backup, const json& body, const User& user, BackupJob& job) {
    std::string name = body.value("name", "");
    name.erase(0, name.find_first_not_of(" \t"));
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    if (name.empty()) throw BadRequest("Give the backup a name");
    if (name.size() > 200) throw BadRequest("The name is too long");
    json config = backup.validateConfig(body.value("config", json::object()), user.id);
    json schedule = backup.validateSchedule(body.value("schedule", json::object()), config);
    job.userId = user.id;
    job.name = name;
    job.config = config.dump();
    job.schedule = schedule.dump();
    job.enabled = body.value("enabled", true);
    job.nextRunAt = job.enabled ? BackupService::nextRunAfter(schedule, static_cast<std::int64_t>(std::time(nullptr))) : 0;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "\"" : ", \"") + n + "\"";
    return out;
}

}  // namespace

void registerApi(httplib::Server& server, Database& db, Auth& auth, MailService& mail, GoogleOAuth& oauth,
                 BackupService& backup, AiSorter& aiSorter, MailCache* mailCache, PushService* push) {
    // CSRF-skydd: ändrande anrop måste bära en egen header, som webbläsare inte skickar cross-site utan CORS.
    server.set_pre_routing_handler([](const Request& req, Response& res) {
        if (req.path.rfind("/api/", 0) != 0) return httplib::Server::HandlerResponse::Unhandled;
        if (req.method != "GET" && req.method != "HEAD" && req.get_header_value("X-Brevduva") != "1") {
            sendError(res, 403, "The request is missing the X-Brevduva header");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Omsluter en handler som kräver inloggning (och eventuellt administratör).
    auto authed = [&auth](UserHandler handler, bool adminOnly = false) {
        return guarded([&auth, handler = std::move(handler), adminOnly](const Request& req, Response& res) {
            auto user = auth.userForToken(sessionToken(req));
            if (!user) throw Unauthorized("Not logged in");
            if (adminOnly && !user->isAdmin) throw Forbidden("Requires administrator privileges");
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
                          {"expiresAt", user ? auth.sessionExpiresAt(sessionToken(req)) : 0},
                      });
    }));

    // Byter till en ny session med full livslängd, så att en klient som används regelbundet (appen) inte loggas ut
    // när de 14 dagarna har gått. Den nya skapas före den gamla tas bort, så ett fel lämnar användaren inloggad.
    server.Post("/api/session/renew", authed([&auth](const Request& req, Response& res, const User& user) {
        std::string token = auth.createSession(user.id);
        auth.destroySession(sessionToken(req));
        setSessionCookie(res, token);
        sendJson(res, {{"ok", true}, {"expiresAt", auth.sessionExpiresAt(token)}});
    }));

    server.Post("/api/setup", guarded([&db, &auth](const Request& req, Response& res) {
        json body = parseBody(req);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        Auth::validateUsername(username);
        Auth::validatePassword(password);
        auto id = db.createFirstAdmin(username, crypto::hashPassword(password));
        if (!id) throw Forbidden("Setup has already been completed");
        std::cout << "[auth] administrator " << username << " was created via the setup page" << std::endl;
        setSessionCookie(res, auth.createSession(*id));
        sendJson(res, {{"authenticated", true}, {"user", userJson(*db.getUser(*id))}}, 201);
    }));

    server.Post("/api/login", guarded([&auth](const Request& req, Response& res) {
        json body = parseBody(req);
        auto user = auth.login(body.value("username", ""), body.value("password", ""), req.remote_addr);
        if (!user) throw Unauthorized("Wrong username or password");
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
            throw BadRequest("The current password is incorrect");
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
        std::cout << "[auth] " << admin.username << " created user " << username << std::endl;
        sendJson(res, userJson(*db.getUser(id)), 201);
    }, true));

    server.Put("/api/users/:id", authed([&db, &auth](const Request& req, Response& res, const User& admin) {
        long long id = pathId(req);
        auto target = db.getUser(id);
        if (!target) throw NotFound("The user does not exist");
        json body = parseBody(req);

        if (body.contains("isAdmin")) {
            bool makeAdmin = body.at("isAdmin").get<bool>();
            if (!makeAdmin && target->isAdmin) {
                if (id == admin.id) throw BadRequest("You cannot remove your own administrator privileges");
                if (db.countAdmins() <= 1) throw BadRequest("There must be at least one administrator");
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
        if (id == admin.id) throw BadRequest("You cannot delete yourself");
        if (!db.getUser(id)) throw NotFound("The user does not exist");
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

    server.Put("/api/accounts/:id", authed([&db, &mail, &oauth, mailCache](const Request& req, Response& res, const User& user) {
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
        if (mailCache) mailCache->accountChanged(a.id);
        sendJson(res, accountJson(a));
    }));

    server.Delete("/api/accounts/:id", authed([&db, &mail, &oauth, &backup](const Request& req, Response& res, const User& user) {
        Account account = ownedAccount(db, pathId(req), user);
        auto jobs = backup.jobsUsingAccount(user.id, account.id);
        if (!jobs.empty()) {
            throw BadRequest("The account is used by the backup " + joinNames(jobs) + ". Delete or change the backup first.");
        }
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
        if (!result) throw NotFound("The sign-in does not exist or has expired");
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
                        throw BadRequest("The file contains no Thunderbird settings (prefs.js)");
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
                       std::string("attachment; filename=\"pigeonpost-settings-") + date + ".zip\"");
        res.set_header("Cache-Control", "no-store");
        res.set_content(zip, "application/zip");
    }));

    // --- Backup ----------------------------------------------------------------
    server.Get("/api/backup", authed([&db, &oauth](const Request&, Response& res, const User& user) {
        json connections = json::array();
        for (const auto& c : db.listDriveConnections(user.id)) connections.push_back(driveConnectionJson(c));
        json jobs = json::array();
        for (const auto& j : db.listBackupJobs(user.id)) jobs.push_back(backupJobJson(db, j));
        json runs = json::array();
        for (const auto& r : db.listBackupRuns(user.id, 30)) runs.push_back(backupRunJson(r));
        sendJson(res, {{"driveConfigured", oauth.configured()},
                       {"driveFolderName", BackupService::kDriveFolderName},
                       {"driveConnections", connections},
                       {"jobs", jobs},
                       {"runs", runs}});
    }));

    server.Post("/api/backup/runs", authed([&backup](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        json config = backup.validateConfig(body.value("config", json::object()), user.id);
        sendJson(res, backupRunJson(backup.enqueue(user.id, 0, config)), 201);
    }));

    server.Post("/api/backup/runs/:id/cancel", authed([&db, &backup](const Request& req, Response& res, const User& user) {
        BackupRun run = ownedBackupRun(db, req, user);
        if (run.status == "queued" || run.status == "running") backup.cancel(run.id);
        sendJson(res, {{"ok", true}});
    }));

    server.Delete("/api/backup/runs/:id", authed([&db, &backup](const Request& req, Response& res, const User& user) {
        BackupRun run = ownedBackupRun(db, req, user);
        if (run.status == "queued" || run.status == "running") throw BadRequest("Cancel the run before deleting it");
        backup.removeFile(run);
        db.deleteBackupRun(run.id);
        sendJson(res, {{"ok", true}});
    }));

    server.Get("/api/backup/runs/:id/download", authed([&db](const Request& req, Response& res, const User& user) {
        BackupRun run = ownedBackupRun(db, req, user);
        if (run.filePath.empty() || run.status != "done") throw NotFound("The backup file no longer exists");
        auto file = std::shared_ptr<std::FILE>(std::fopen(run.filePath.c_str(), "rb"), [](std::FILE* f) {
            if (f) std::fclose(f);
        });
        if (!file) throw NotFound("The backup file no longer exists");
        json config = parseStored(run.config);
        char date[16];
        std::time_t created = static_cast<std::time_t>(run.createdAt);
        std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&created));
        std::string filename = "pigeonpost-backup-" + config.value("sourceEmail", std::string("account")) + "-" + date + ".zip";
        res.set_header("Content-Disposition", "attachment; filename*=UTF-8''" + encodeFilename(filename));
        res.set_header("Cache-Control", "no-store");
        // Filen kan vara flera gigabyte, så den skickas i bitar i stället för att läsas in i minnet.
        res.set_content_provider(
            static_cast<std::size_t>(run.fileSize), "application/zip",
            [file](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                char buffer[64 * 1024];
                if (fseeko(file.get(), static_cast<off_t>(offset), SEEK_SET) != 0) return false;
                std::size_t n = std::fread(buffer, 1, std::min(length, sizeof(buffer)), file.get());
                return n > 0 && sink.write(buffer, n);
            });
    }));

    server.Post("/api/backup/jobs", authed([&db, &backup](const Request& req, Response& res, const User& user) {
        BackupJob job;
        backupJobFromJson(backup, parseBody(req), user, job);
        job.id = db.createBackupJob(job);
        sendJson(res, backupJobJson(db, *db.getBackupJob(job.id)), 201);
    }));

    server.Put("/api/backup/jobs/:id", authed([&db, &backup](const Request& req, Response& res, const User& user) {
        BackupJob job = ownedBackupJob(db, req, user);
        backupJobFromJson(backup, parseBody(req), user, job);
        db.updateBackupJob(job);
        sendJson(res, backupJobJson(db, *db.getBackupJob(job.id)));
    }));

    server.Delete("/api/backup/jobs/:id", authed([&db](const Request& req, Response& res, const User& user) {
        BackupJob job = ownedBackupJob(db, req, user);
        if (db.hasActiveBackupRun(job.id)) throw BadRequest("The backup is running right now. Cancel it first.");
        db.deleteBackupJob(job.id);
        sendJson(res, {{"ok", true}});
    }));

    server.Post("/api/backup/jobs/:id/run", authed([&db, &backup](const Request& req, Response& res, const User& user) {
        BackupJob job = ownedBackupJob(db, req, user);
        json config = backup.validateConfig(parseStored(job.config), user.id);
        sendJson(res, backupRunJson(backup.enqueue(user.id, job.id, config)), 201);
    }));

    server.Post("/api/backup/drive/start", authed([&oauth](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        auto [state, url] = oauth.start(user.id, body.value("loginHint", ""), oauth.redirectUri(req.get_header_value("Host")),
                                        GoogleOAuth::Purpose::Drive);
        sendJson(res, {{"state", state}, {"url", url}});
    }));

    server.Post("/api/backup/drive", authed([&db, &oauth](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        std::string state = body.value("oauthState", "");
        auto status = oauth.status(state, user.id);
        if (!status || !status->done || !status->error.empty() || status->purpose != GoogleOAuth::Purpose::Drive) {
            throw BadRequest("The Google sign-in is not complete or has expired. Try again.");
        }
        auto result = oauth.take(state, user.id);
        if (!result) throw BadRequest("The Google sign-in has already been used. Try again.");
        // En ny inloggning för samma Google-konto förnyar den befintliga kopplingen, så att scheman fortsätter fungera.
        for (const auto& existing : db.listDriveConnections(user.id)) {
            if (existing.email == result->email) {
                db.setDriveRefreshToken(existing.id, result->refreshToken);
                oauth.forgetDrive(existing.id);
                sendJson(res, driveConnectionJson(existing));
                return;
            }
        }
        DriveConnection connection;
        connection.userId = user.id;
        connection.email = result->email;
        connection.refreshToken = result->refreshToken;
        long long id = db.createDriveConnection(connection);
        sendJson(res, driveConnectionJson(*db.getDriveConnection(id)), 201);
    }));

    server.Delete("/api/backup/drive/:id", authed([&db, &oauth, &backup](const Request& req, Response& res, const User& user) {
        auto connection = db.getDriveConnection(pathId(req));
        if (!connection || connection->userId != user.id) throw NotFound("The connection does not exist");
        auto jobs = backup.jobsUsingDrive(user.id, connection->id);
        if (!jobs.empty()) {
            throw BadRequest("The Google Drive connection is used by the backup " + joinNames(jobs) +
                             ". Delete or change the backup first.");
        }
        db.deleteDriveConnection(connection->id);
        oauth.forgetDrive(connection->id);
        if (!googleAccountInUse(db, user.id, connection->email, 0, connection->id)) oauth.revoke(connection->refreshToken);
        sendJson(res, {{"ok", true}});
    }));

    // --- Avsändaridentiteter ------------------------------------------------
    server.Post("/api/accounts/:id/identities/test",
                authed([&db, &mail](const Request& req, Response& res, const User& user) {
                    Account account = ownedAccount(db, pathId(req), user);
                    json body = parseBody(req);
                    std::optional<Identity> existing;
                    if (body.contains("id") && !body["id"].is_null()) {
                        auto found = db.getIdentity(body["id"].get<long long>());
                        if (!found || found->accountId != account.id) throw NotFound("The sender does not exist");
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
                sendJson(res, {{"error", "The SMTP test failed"}, {"test", test}}, 400);
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
                authed([&db, &mail, &aiSorter](const Request& req, Response& res, const User& user) {
                    json body = parseBody(req);
                    long long accountId = ownedAccountId(db, req, user);
                    std::string from = body.at("from").get<std::string>();
                    std::string to = body.at("to").get<std::string>();
                    // Namnbyte och flytt är samma sak i IMAP: mappen och dess undermappar får en ny sökväg.
                    mail.renameFolder(accountId, from, to);
                    aiSorter.folderRenamed(accountId, from, to);
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
        q.beforeUid = static_cast<std::uint32_t>(std::max(0, intParam(req, "beforeUid", 0)));
        q.sort = req.has_param("sort") ? req.get_param_value("sort") : "date";
        if (!isSortKey(q.sort)) throw BadRequest("Unknown sort order: " + q.sort);
        q.descending = req.get_param_value("order") != "asc";
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
                   // Bilagor öppnas i en ny flik. Bara typer som webbläsaren visar utan att köra skript får
                   // visas direkt (rasterbilder, PDF och ren text); allt annat skickas som nedladdning för att
                   // undvika XSS via bilagor.
                   static const std::set<std::string> inlineSafe = {"image/png", "image/jpeg",      "image/gif",
                                                                    "image/webp", "application/pdf", "text/plain"};
                   std::string mediaType = att.contentType.substr(0, att.contentType.find(';'));
                   while (!mediaType.empty() && mediaType.back() == ' ') mediaType.pop_back();
                   std::transform(mediaType.begin(), mediaType.end(), mediaType.begin(),
                                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                   bool inlineView = req.get_param_value("download") != "1" && inlineSafe.count(mediaType);
                   std::string type = inlineView ? mediaType : "application/octet-stream";
                   res.set_header("Content-Disposition", std::string(inlineView ? "inline" : "attachment") +
                                                             "; filename*=UTF-8''" + encodeFilename(att.filename));
                   // Webbläsarens PDF-visare räknas som insticksprogram och blockeras av sandbox-direktivet;
                   // den kör ändå isolerat utan åtkomst till sidan, så PDF får en CSP utan sandbox.
                   bool pdf = inlineView && mediaType == "application/pdf";
                   res.set_header("Content-Security-Policy", pdf ? "default-src 'none'" : "default-src 'none'; sandbox");
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

    // --- Mejlcache ---------------------------------------------------------------
    server.Get("/api/cache", authed([&db, mailCache](const Request&, Response& res, const User& user) {
        sendJson(res, {{"enabled", mailCache != nullptr},
                       {"accounts", mailCache ? mailCache->status(db.listAccounts(user.id)) : json::array()}});
    }));

    // Sökning över flera mappar och konton. Går mot mejlcachen, så bara det som har hunnit synkas hittas.
    // body: {accountId?, folder?, q, from, to, after, before, unread, flagged, attachments, includeTrash, page, pageSize}
    server.Post("/api/search", authed([&db, mailCache](const Request& req, Response& res, const User& user) {
        if (!mailCache) throw BadRequest("Searching all folders requires the mail cache to be enabled (MAIL_CACHE)");
        json body = parseBody(req);
        auto text = [&](const char* key, std::size_t maxBytes) {
            auto it = body.find(key);
            return it != body.end() && it->is_string() ? textutil::truncateUtf8Bytes(it->get<std::string>(), maxBytes)
                                                       : std::string();
        };
        auto number = [&](const char* key) {
            auto it = body.find(key);
            return it != body.end() && it->is_number_integer() ? it->get<std::int64_t>() : std::int64_t{0};
        };
        auto flag = [&](const char* key) {
            auto it = body.find(key);
            return it != body.end() && it->is_boolean() && it->get<bool>();
        };

        CacheSearchQuery q;
        if (std::int64_t accountId = number("accountId"); accountId > 0) {
            q.accountIds.push_back(ownedAccount(db, accountId, user).id);
        } else {
            for (const auto& a : db.listAccounts(user.id)) q.accountIds.push_back(a.id);
        }
        q.folder = text("folder", 1000);
        q.includeTrash = flag("includeTrash");
        q.skipSent = flag("skipSent");
        q.terms = textutil::parseSearchTerms(text("q", 1000));
        q.from = text("from", 200);
        q.to = text("to", 200);
        q.after = std::max<std::int64_t>(0, number("after"));
        q.before = std::max<std::int64_t>(0, number("before"));
        q.unreadOnly = flag("unread");
        q.flaggedOnly = flag("flagged");
        q.attachmentsOnly = flag("attachments");
        q.page = static_cast<int>(std::clamp<std::int64_t>(number("page"), 0, 100000));
        q.pageSize = static_cast<int>(std::clamp<std::int64_t>(number("pageSize") ? number("pageSize") : 50, 1, 200));
        q.sort = body.contains("sort") && body["sort"].is_string() ? body["sort"].get<std::string>() : "date";
        if (!isSortKey(q.sort)) throw BadRequest("Unknown sort order: " + q.sort);
        q.descending = text("order", 10) != "asc";
        sendJson(res, mailCache->search(q));
    }));

    // Ändringar i kontots cache sedan ett ändringsnummer, för appen: ?since=<modseq>&limit=<rader>.
    // since=0 ger mapplistan och det aktuella numret; första fyllningen görs med /messages per mapp.
    server.Get("/api/accounts/:id/changes", authed([&db, mailCache](const Request& req, Response& res, const User& user) {
        if (!mailCache) throw BadRequest("The change list requires the mail cache to be enabled (MAIL_CACHE)");
        long long accountId = ownedAccountId(db, req, user);
        std::int64_t since = 0;
        if (req.has_param("since")) {
            try {
                since = std::stoll(req.get_param_value("since"));
            } catch (...) {
                throw BadRequest("Invalid value for since");
            }
        }
        int limit = std::clamp(intParam(req, "limit", 500), 1, 2000);
        sendJson(res, mailCache->changes(accountId, since, limit));
    }));

    // Synkar kontot nu (knappen Hämta) och väntar en kort stund på resultatet.
    server.Post("/api/accounts/:id/sync", authed([&db, mailCache](const Request& req, Response& res, const User& user) {
        Account account = ownedAccount(db, pathId(req), user);
        if (!mailCache) {
            sendJson(res, {{"enabled", false}});
            return;
        }
        sendJson(res, {{"enabled", true}, {"account", mailCache->syncNow(account, 8)}});
    }));

    // --- Enheter för push-notiser (Android-appen) ---------------------------------
    // PUT body: {token, name, platform, appVersion, notify}. notify = {"<kontoId>": ["INBOX", ...]} väljer mappar
    // per konto; ett konto som saknas ger notiser för inkorgen. En token som redan finns flyttas till den som loggar in.
    auto deviceToken = [](const json& body) {
        std::string token = body.value("token", std::string());
        if (token.empty() || token.size() > 4096 || token.find_first_of(" \t\r\n") != std::string::npos) {
            throw BadRequest("Invalid device token");
        }
        return token;
    };
    auto deviceJson = [](const Device& d) {
        json notify = json::parse(d.notify, nullptr, false);
        return json{{"id", d.id},
                    {"name", d.name},
                    {"platform", d.platform},
                    {"appVersion", d.appVersion},
                    {"notify", notify.is_object() ? notify : json::object()},
                    {"createdAt", d.createdAt},
                    {"lastSeenAt", d.lastSeenAt}};
    };
    server.Get("/api/devices", authed([&db, push, deviceJson](const Request&, Response& res, const User& user) {
        json devices = json::array();
        for (const auto& d : db.listDevices(user.id)) devices.push_back(deviceJson(d));
        sendJson(res, {{"pushEnabled", push != nullptr}, {"devices", devices}});
    }));
    server.Put("/api/devices", authed([&db, deviceToken](const Request& req, Response& res, const User& user) {
        json body = parseBody(req);
        Device d;
        d.userId = user.id;
        d.token = deviceToken(body);
        d.name = textutil::truncateUtf8Bytes(body.value("name", std::string()), 100);
        d.platform = textutil::truncateUtf8Bytes(body.value("platform", std::string()), 16);
        d.appVersion = textutil::truncateUtf8Bytes(body.value("appVersion", std::string()), 32);
        if (body.contains("notify") && !body["notify"].is_null()) {
            const json& notify = body["notify"];
            if (!notify.is_object()) throw BadRequest("notify must be an object keyed by account id");
            json clean = json::object();
            std::size_t folders = 0;
            for (auto it = notify.begin(); it != notify.end(); ++it) {
                long long accountId = 0;
                try {
                    accountId = std::stoll(it.key());
                } catch (...) {
                    throw BadRequest("Invalid account id in notify: " + it.key());
                }
                ownedAccount(db, accountId, user);
                if (!it->is_array()) throw BadRequest("notify: the folders for an account must be a list");
                json list = json::array();
                for (const auto& f : *it) {
                    if (!f.is_string() || f.get<std::string>().empty() || f.get<std::string>().size() > 1000) {
                        throw BadRequest("notify: invalid folder name");
                    }
                    if (++folders > 500) throw BadRequest("notify: too many folders");
                    list.push_back(f);
                }
                clean[it.key()] = std::move(list);
            }
            d.notify = clean.dump();
        }
        db.upsertDevice(d);
        sendJson(res, {{"ok", true}});
    }));
    server.Post("/api/devices/delete", authed([&db, deviceToken](const Request& req, Response& res, const User& user) {
        db.deleteDevice(user.id, deviceToken(parseBody(req)));
        sendJson(res, {{"ok", true}});
    }));

    // --- Avsändare vars fjärrinnehåll alltid visas -------------------------------
    // body: {sender}. Alla tre svarar med hela listan.
    auto remoteSender = [](const Request& req) {
        std::string sender = parseBody(req).value("sender", std::string());
        std::transform(sender.begin(), sender.end(), sender.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto at = sender.find('@');
        if (sender.size() > 320 || at == std::string::npos || at == 0 || at + 1 >= sender.size() ||
            sender.find_first_of(" \t\r\n<>,;") != std::string::npos) {
            throw BadRequest("Invalid email address");
        }
        return sender;
    };
    server.Get("/api/remote-senders", authed([&db](const Request&, Response& res, const User& user) {
        sendJson(res, {{"senders", db.listRemoteSenders(user.id)}});
    }));
    server.Post("/api/remote-senders", authed([&db, remoteSender](const Request& req, Response& res, const User& user) {
        db.addRemoteSender(user.id, remoteSender(req));
        sendJson(res, {{"senders", db.listRemoteSenders(user.id)}});
    }));
    server.Post("/api/remote-senders/delete", authed([&db, remoteSender](const Request& req, Response& res, const User& user) {
        db.deleteRemoteSender(user.id, remoteSender(req));
        sendJson(res, {{"senders", db.listRemoteSenders(user.id)}});
    }));

    // --- AI-sortering ---------------------------------------------------------
    server.Get("/api/ai", authed([&aiSorter](const Request&, Response& res, const User& user) {
        sendJson(res, aiSorter.state(user.id));
    }));

    server.Put("/api/ai", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.save(user.id, parseBody(req)));
    }));

    server.Delete("/api/ai", authed([&aiSorter](const Request&, Response& res, const User& user) {
        aiSorter.remove(user.id);
        sendJson(res, {{"ok", true}});
    }));

    server.Post("/api/ai/test", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.test(user.id, parseBody(req)));
    }));

    // Gör om en fråga i fritext till sökfilter. Bara frågan och mapparnas namn skickas till AI-tjänsten, inga mejl.
    server.Post("/api/ai/search", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.interpretSearch(user.id, parseBody(req)));
    }));

    server.Get("/api/ai/auto", authed([&aiSorter](const Request&, Response& res, const User& user) {
        sendJson(res, aiSorter.autoState(user.id));
    }));

    server.Put("/api/accounts/:id/ai-auto", authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
        long long accountId = ownedAccountId(db, req, user);
        sendJson(res, aiSorter.saveAutoRule(user.id, accountId, parseBody(req)));
    }));

    server.Put("/api/accounts/:id/ai-sort-targets",
               authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
                   long long accountId = ownedAccountId(db, req, user);
                   sendJson(res, aiSorter.saveSortExcluded(user.id, accountId, parseBody(req)));
               }));

    server.Put("/api/accounts/:id/ai-sort-cursor",
               authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
                   long long accountId = ownedAccountId(db, req, user);
                   sendJson(res, aiSorter.saveSortCursor(user.id, accountId, parseBody(req)));
               }));

    // Det sorteringen har lärt sig: avsändarregler, regelförslag och rättningar
    server.Get("/api/ai/learning", authed([&aiSorter](const Request&, Response& res, const User& user) {
        sendJson(res, aiSorter.learningState(user.id));
    }));

    server.Post("/api/accounts/:id/ai-feedback", authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
        long long accountId = ownedAccountId(db, req, user);
        sendJson(res, aiSorter.feedback(user.id, accountId, parseBody(req)));
    }));

    server.Post("/api/accounts/:id/ai-rules", authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
        long long accountId = ownedAccountId(db, req, user);
        sendJson(res, aiSorter.saveSenderRule(user.id, accountId, parseBody(req)));
    }));

    server.Delete("/api/ai/rules/:id", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.deleteSenderRule(user.id, pathId(req)));
    }));

    server.Delete("/api/ai/examples/:id", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.deleteExample(user.id, pathId(req)));
    }));

    server.Delete("/api/ai/examples", authed([&aiSorter](const Request&, Response& res, const User& user) {
        sendJson(res, aiSorter.clearExamples(user.id));
    }));

    // Sortering av en hel mapp i bakgrunden
    server.Get("/api/ai/jobs", authed([&aiSorter](const Request&, Response& res, const User& user) {
        sendJson(res, aiSorter.jobsState(user.id));
    }));

    server.Post("/api/accounts/:id/ai-jobs", authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
        long long accountId = ownedAccountId(db, req, user);
        sendJson(res, aiSorter.startJob(user.id, accountId, parseBody(req)));
    }));

    server.Post("/api/ai/jobs/:id/:action", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.jobAction(user.id, pathId(req), req.path_params.at("action")));
    }));

    server.Post("/api/ai/log/:id/undo", authed([&aiSorter](const Request& req, Response& res, const User& user) {
        sendJson(res, aiSorter.undo(user.id, pathId(req)));
    }));

    // Föreslår målmappar. Inget flyttas; klienten flyttar det användaren godkänner med /messages/move.
    server.Post("/api/accounts/:id/ai-sort",
                authed([&db, &aiSorter](const Request& req, Response& res, const User& user) {
                    long long accountId = ownedAccountId(db, req, user);
                    sendJson(res, aiSorter.suggest(user.id, accountId, parseBody(req)));
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
