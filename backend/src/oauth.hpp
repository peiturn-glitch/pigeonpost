#pragma once

#include "config.hpp"
#include "db.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace brevduva {

// Google-inloggning (OAuth 2.0 med PKCE) för Gmail via IMAP/SMTP med SASL XOAUTH2.
class GoogleOAuth {
public:
    explicit GoogleOAuth(const Config& config);

    bool configured() const { return !clientId_.empty() && !clientSecret_.empty(); }

    // Callback-adressen som ska registreras hos Google. Utan PUBLIC_URL används förfrågans Host.
    std::string redirectUri(const std::string& requestHost) const;

    // Vad inloggningen ska användas till. Styr vilka behörigheter som begärs.
    enum class Purpose { Mail, Drive };

    // Startar en inloggning för användaren och returnerar {state, url}.
    std::pair<std::string, std::string> start(long long userId, const std::string& loginHint,
                                              const std::string& redirectUri, Purpose purpose = Purpose::Mail);

    // Tar emot Googles omdirigering. Returnerar false om state är okänt eller har gått ut.
    bool handleCallback(const std::string& state, const std::string& code, const std::string& error);

    struct Result {
        Purpose purpose = Purpose::Mail;
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
    // En giltig access-token för en Google Drive-koppling.
    std::string accessToken(const DriveConnection& connection);
    void forget(long long accountId);
    void forgetDrive(long long connectionId);
    void revoke(const std::string& refreshToken);

private:
    using Clock = std::chrono::steady_clock;

    struct Pending {
        long long userId = 0;
        Purpose purpose = Purpose::Mail;
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
    // Hämtar en access-token från cachen eller förnyar den med refresh-token. label används i felmeddelanden.
    std::string cachedAccessToken(const std::string& cacheKey, const std::string& refreshToken,
                                  const std::string& label);

    std::string clientId_;
    std::string clientSecret_;
    std::string publicUrl_;
    std::string authUrl_;
    std::string tokenUrl_;
    std::string revokeUrl_;

    std::mutex mutex_;
    std::map<std::string, Pending> pending_;
    std::map<std::string, CachedToken> tokens_;  // "account:<id>" eller "drive:<id>"
};

}  // namespace brevduva
