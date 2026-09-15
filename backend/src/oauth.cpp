#include "oauth.hpp"

#include "crypto.hpp"
#include "errors.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <iostream>
#include <memory>

namespace brevduva {

namespace {

using json = nlohmann::json;

constexpr auto kLoginTimeout = std::chrono::minutes(10);
constexpr auto kTokenMargin = std::chrono::seconds(60);
const char* kScopes = "https://mail.google.com/ openid email profile";

std::string base64Url(const std::string& bytes) {
    std::string s = crypto::base64Encode(bytes);
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string base64UrlDecode(std::string s) {
    for (char& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (s.size() % 4 != 0) s += '=';
    return crypto::base64Decode(s);
}

std::string urlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

std::string formBody(const std::map<std::string, std::string>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) out += (out.empty() ? "" : "&") + urlEncode(k) + "=" + urlEncode(v);
    return out;
}

std::size_t collect(char* data, std::size_t size, std::size_t n, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * n);
    return size * n;
}

// POST med formulärdata. Returnerar HTTP-status och svarskropp.
std::pair<long, std::string> postForm(const std::string& url, const std::string& body) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init misslyckades");
    std::string response;
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    CURL* c = curl.get();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        throw NetError(std::string("Kunde inte nå Google: ") + (errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc)));
    }
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    return {status, response};
}

std::string describeTokenError(const json& body) {
    std::string error = body.value("error", "okänt fel");
    std::string description = body.value("error_description", "");
    return description.empty() ? error : error + ": " + description;
}

}  // namespace

GoogleOAuth::GoogleOAuth(const Config& config)
    : clientId_(config.googleClientId),
      clientSecret_(config.googleClientSecret),
      publicUrl_(config.publicUrl),
      authUrl_(config.googleAuthUrl),
      tokenUrl_(config.googleTokenUrl),
      revokeUrl_(config.googleRevokeUrl) {}

std::string GoogleOAuth::redirectUri(const std::string& requestHost) const {
    std::string base = publicUrl_.empty() ? "http://" + requestHost : publicUrl_;
    return base + "/api/oauth/google/callback";
}

void GoogleOAuth::purgeExpired() {
    auto now = Clock::now();
    std::erase_if(pending_, [now](const auto& entry) { return entry.second.expires < now; });
}

std::pair<std::string, std::string> GoogleOAuth::start(long long userId, const std::string& loginHint,
                                                       const std::string& redirectUri) {
    if (!configured()) throw BadRequest("Google-inloggning är inte konfigurerad på servern");

    std::string state = crypto::toHex(crypto::randomBytes(24));
    Pending p;
    p.userId = userId;
    p.verifier = base64Url(crypto::randomBytes(32));
    p.redirectUri = redirectUri;
    p.expires = Clock::now() + kLoginTimeout;
    std::string challenge = base64Url(crypto::sha256(p.verifier));

    // access_type=offline och prompt=consent ger en refresh-token även vid ny inloggning.
    std::string url = authUrl_ + "?" +
                      formBody({{"client_id", clientId_},
                                {"redirect_uri", redirectUri},
                                {"response_type", "code"},
                                {"scope", kScopes},
                                {"access_type", "offline"},
                                {"prompt", "consent"},
                                {"state", state},
                                {"code_challenge", challenge},
                                {"code_challenge_method", "S256"},
                                {"include_granted_scopes", "true"}});
    if (!loginHint.empty()) url += "&login_hint=" + urlEncode(loginHint);

    std::lock_guard lock(mutex_);
    purgeExpired();
    pending_[state] = std::move(p);
    return {state, url};
}

bool GoogleOAuth::handleCallback(const std::string& state, const std::string& code, const std::string& error) {
    Pending pending;
    {
        std::lock_guard lock(mutex_);
        purgeExpired();
        auto it = pending_.find(state);
        if (it == pending_.end() || it->second.result.done) return false;
        pending = it->second;
    }

    Result result;
    result.done = true;
    try {
        if (!error.empty()) {
            result.error = error == "access_denied" ? "Inloggningen avbröts." : "Google avvisade inloggningen (" + error + ").";
        } else if (code.empty()) {
            result.error = "Svaret från Google saknade kod.";
        } else {
            auto [status, body] = postForm(tokenUrl_, formBody({{"grant_type", "authorization_code"},
                                                                {"code", code},
                                                                {"redirect_uri", pending.redirectUri},
                                                                {"client_id", clientId_},
                                                                {"client_secret", clientSecret_},
                                                                {"code_verifier", pending.verifier}}));
            json j = json::parse(body, nullptr, false);
            if (status != 200 || j.is_discarded()) {
                result.error = "Tokenutbytet misslyckades: " + (j.is_discarded() ? "ogiltigt svar" : describeTokenError(j));
            } else {
                result.accessToken = j.value("access_token", "");
                result.refreshToken = j.value("refresh_token", "");
                // id_token kommer direkt från Googles token-endpoint över TLS, så payloaden kan läsas utan
                // signaturkontroll (OpenID Connect Core 3.1.3.7).
                std::string idToken = j.value("id_token", "");
                std::size_t a = idToken.find('.');
                std::size_t b = idToken.find('.', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    json claims = json::parse(base64UrlDecode(idToken.substr(a + 1, b - a - 1)), nullptr, false);
                    if (!claims.is_discarded()) {
                        if (claims.value("email_verified", false)) result.email = claims.value("email", "");
                        result.name = claims.value("name", "");
                    }
                }
                if (result.refreshToken.empty()) result.error = "Google skickade ingen refresh-token.";
                else if (result.email.empty()) result.error = "Google skickade ingen verifierad e-postadress.";
            }
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    std::lock_guard lock(mutex_);
    auto it = pending_.find(state);
    if (it == pending_.end()) return false;
    it->second.result = std::move(result);
    return true;
}

std::optional<GoogleOAuth::Result> GoogleOAuth::status(const std::string& state, long long userId) {
    std::lock_guard lock(mutex_);
    purgeExpired();
    auto it = pending_.find(state);
    if (it == pending_.end() || it->second.userId != userId) return std::nullopt;
    return it->second.result;
}

std::optional<GoogleOAuth::Result> GoogleOAuth::take(const std::string& state, long long userId) {
    std::lock_guard lock(mutex_);
    purgeExpired();
    auto it = pending_.find(state);
    if (it == pending_.end() || it->second.userId != userId) return std::nullopt;
    if (!it->second.result.done || !it->second.result.error.empty()) return std::nullopt;
    Result result = std::move(it->second.result);
    pending_.erase(it);
    return result;
}

std::string GoogleOAuth::accessToken(const Account& account) {
    if (!account.oauthAccessToken.empty()) return account.oauthAccessToken;
    {
        std::lock_guard lock(mutex_);
        auto it = tokens_.find(account.id);
        if (it != tokens_.end() && it->second.expires - kTokenMargin > Clock::now()) return it->second.token;
    }
    if (!configured()) throw MailServerError("Google-inloggning är inte längre konfigurerad på servern");
    if (account.oauthRefreshToken.empty()) throw MailServerError("Kontot saknar Google-inloggning. Logga in med Google igen.");

    auto [status, body] = postForm(tokenUrl_, formBody({{"grant_type", "refresh_token"},
                                                        {"refresh_token", account.oauthRefreshToken},
                                                        {"client_id", clientId_},
                                                        {"client_secret", clientSecret_}}));
    json j = json::parse(body, nullptr, false);
    if (status != 200 || j.is_discarded() || !j.contains("access_token")) {
        std::string reason = j.is_discarded() ? "ogiltigt svar" : describeTokenError(j);
        if (!j.is_discarded() && j.value("error", "") == "invalid_grant") {
            throw MailServerError("Google-inloggningen för " + account.email +
                                  " har upphört eller återkallats. Öppna kontoinställningarna och logga in med Google igen.");
        }
        throw MailServerError("Kunde inte förnya Google-inloggningen: " + reason);
    }

    CachedToken cached{j.value("access_token", ""), Clock::now() + std::chrono::seconds(j.value("expires_in", 3600))};
    std::lock_guard lock(mutex_);
    tokens_[account.id] = cached;
    return cached.token;
}

void GoogleOAuth::forget(long long accountId) {
    std::lock_guard lock(mutex_);
    tokens_.erase(accountId);
}

void GoogleOAuth::revoke(const std::string& refreshToken) {
    if (refreshToken.empty()) return;
    try {
        postForm(revokeUrl_, formBody({{"token", refreshToken}}));
    } catch (const std::exception& e) {
        std::cerr << "[oauth] kunde inte återkalla token: " << e.what() << "\n";
    }
}

}  // namespace brevduva
