#include "auth.hpp"

#include "crypto.hpp"
#include "errors.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace brevduva {

namespace {

constexpr int kMaxFailuresPerUser = 10;
constexpr int kMaxFailuresPerClient = 30;
constexpr auto kFailureWindow = std::chrono::minutes(15);

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Auth::Auth(Database& db) : db_(db), dummyHash_(crypto::hashPassword(crypto::toHex(crypto::randomBytes(16)))) {
    db_.purgeExpiredSessions(nowSeconds());
}

void Auth::validateUsername(const std::string& username) {
    if (username.empty() || username.size() > 64) throw BadRequest("Användarnamnet måste vara 1–64 tecken");
    bool ok = std::all_of(username.begin(), username.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '@';
    });
    if (!ok) throw BadRequest("Användarnamnet får bara innehålla a–z, 0–9 och . _ - @");
}

void Auth::validatePassword(const std::string& password) {
    if (password.size() < 8) throw BadRequest("Lösenordet måste vara minst 8 tecken");
    if (password.size() > 1024) throw BadRequest("Lösenordet är för långt");
}

bool Auth::limited(const std::string& key) {
    auto it = failures_.find(key);
    if (it == failures_.end()) return false;
    if (Clock::now() - it->second.windowStart > kFailureWindow) {
        failures_.erase(it);
        return false;
    }
    int max = key.rfind("user:", 0) == 0 ? kMaxFailuresPerUser : kMaxFailuresPerClient;
    return it->second.count >= max;
}

void Auth::recordFailure(const std::string& key) {
    // Håll tabellen liten även om någon provar tusentals påhittade användarnamn.
    if (failures_.size() > 10000) {
        auto now = Clock::now();
        std::erase_if(failures_, [now](const auto& entry) { return now - entry.second.windowStart > kFailureWindow; });
    }
    auto& f = failures_[key];
    if (f.count == 0 || Clock::now() - f.windowStart > kFailureWindow) {
        f.count = 0;
        f.windowStart = Clock::now();
    }
    ++f.count;
}

std::optional<User> Auth::login(const std::string& username, const std::string& password,
                                const std::string& clientKey) {
    std::string userKey = "user:" + lower(username);
    std::string ipKey = "client:" + clientKey;
    {
        std::lock_guard lock(mutex_);
        if (limited(userKey) || limited(ipKey)) {
            throw TooManyRequests("För många misslyckade inloggningar. Försök igen om en stund.");
        }
    }

    auto found = db_.findUserForLogin(username);
    bool ok = crypto::verifyPassword(password, found ? found->second : dummyHash_) && found.has_value();

    std::lock_guard lock(mutex_);
    if (!ok) {
        recordFailure(userKey);
        recordFailure(ipKey);
        return std::nullopt;
    }
    failures_.erase(userKey);
    return found->first;
}

bool Auth::checkPassword(long long userId, const std::string& password) {
    auto user = db_.getUser(userId);
    if (!user) return false;
    auto found = db_.findUserForLogin(user->username);
    return found && crypto::verifyPassword(password, found->second);
}

std::string Auth::createSession(long long userId) {
    std::string token = crypto::toHex(crypto::randomBytes(32));
    std::int64_t now = nowSeconds();
    db_.purgeExpiredSessions(now);
    db_.createSession(crypto::toHex(crypto::sha256(token)), userId, now + kSessionDays * 24 * 3600);
    return token;
}

std::optional<User> Auth::userForToken(const std::string& token) {
    if (token.size() != 64) return std::nullopt;
    return db_.userForSession(crypto::toHex(crypto::sha256(token)), nowSeconds());
}

void Auth::destroySession(const std::string& token) {
    if (!token.empty()) db_.deleteSession(crypto::toHex(crypto::sha256(token)));
}

void Auth::destroyOtherSessions(long long userId, const std::string& keepToken) {
    db_.deleteSessionsForUser(userId, keepToken.empty() ? "" : crypto::toHex(crypto::sha256(keepToken)));
}

}  // namespace brevduva
