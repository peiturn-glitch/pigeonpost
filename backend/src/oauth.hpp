#pragma once

#include "config.hpp"
#include "db.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace tbk {

// Google-inloggning (OAuth 2.0 med PKCE) för Gmail via IMAP/SMTP med SASL XOAUTH2.
class GoogleOAuth {
public:
    explicit GoogleOAuth(const Config& config);

    bool configured() const { return !clientId_.empty() && !clientSecret_.empty(); }

    // Callback-adressen som ska registreras hos Google. Utan PUBLIC_URL används förfrågans Host.
    std::string redirectUri(const std::string& requestHost) const;

    // Startar en inloggning för användaren och returnerar {state, url}.
    std::pair<std::string, std::string> start(long long userId, const std::string& loginHint,
                                              const std::string& redirectUri);

    // Tar emot Googles omdirigering. Returnerar false om state är okänt eller har gått ut.
    bool handleCallback(const std::string& state, const std::string& code, const std::string& error);

    struct Result {
        bool done = false;
        std::string error;  // Tomt om inloggningen lyckades.
        std::string email;
        std::string name;
        std::string refreshToken;
        std::string accessToken;
    };

    // Status för en påbörjad inloggning. Kräver att den tillhör användaren.
    std::optional<Result> status(const std::string& state, long long userId);
    // Hämtar och förbrukar ett lyckat resultat, t.ex. när kontot sparas.
    std::optional<Result> take(const std::string& state, long long userId);

    // En giltig access-token för kontot. Förnyas med refresh-token vid behov.
    // Kastar MailServerError om Google har återkallat inloggningen.
    std::string accessToken(const Account& account);
    void forget(long long accountId);
    void revoke(const std::string& refreshToken);

private:
    using Clock = std::chrono::steady_clock;

    struct Pending {
        long long userId = 0;
        std::string verifier;
        std::string redirectUri;
        Clock::time_point expires;
        Result result;
    };

    struct CachedToken {
        std::string token;
        Clock::time_point expires;
    };

    void purgeExpired();

    std::string clientId_;
    std::string clientSecret_;
    std::string publicUrl_;
    std::string authUrl_;
    std::string tokenUrl_;
    std::string revokeUrl_;

    std::mutex mutex_;
    std::map<std::string, Pending> pending_;
    std::map<long long, CachedToken> tokens_;
};

}  // namespace tbk
