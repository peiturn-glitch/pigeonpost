#pragma once

#include "db.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace brevduva {

// Fleranvändarinloggning. Sessionstoken ligger i en HttpOnly-cookie; databasen lagrar bara dess SHA-256.
class Auth {
public:
    explicit Auth(Database& db);

    bool setupRequired() { return db_.countUsers() == 0; }

    // Kontrollerar användarnamn och lösenord. clientKey (t.ex. IP) används för att bromsa gissningar.
    // Kastar TooManyRequests vid för många misslyckade försök.
    std::optional<User> login(const std::string& username, const std::string& password, const std::string& clientKey);
    bool checkPassword(long long userId, const std::string& password);

    std::string createSession(long long userId);
    std::optional<User> userForToken(const std::string& token);
    // När sessionen går ut (unixtid), eller 0 om den inte finns.
    std::int64_t sessionExpiresAt(const std::string& token);
    void destroySession(const std::string& token);
    void destroyOtherSessions(long long userId, const std::string& keepToken);

    static void validateUsername(const std::string& username);
    static void validatePassword(const std::string& password);

    static constexpr const char* kCookieName = "brevduva_session";
    static constexpr int kSessionDays = 14;

private:
    using Clock = std::chrono::steady_clock;
    struct Failures {
        int count = 0;
        Clock::time_point windowStart;
    };

    bool limited(const std::string& key);
    void recordFailure(const std::string& key);

    Database& db_;
    std::string dummyHash_;  // Verifieras för okända användare så att svarstiden inte avslöjar dem.
    std::map<std::string, Failures> failures_;
    std::mutex mutex_;
};

}  // namespace brevduva
