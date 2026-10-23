#include "push.hpp"

#include "crypto.hpp"
#include "text_util.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

namespace brevduva {

namespace {

using json = nlohmann::json;

constexpr auto kCoalesce = std::chrono::seconds(2);       // Samla ihop en skur av nya mejl till ett utskick.
constexpr auto kTokenMargin = std::chrono::seconds(60);   // Hämta ny access-token så här långt före utgången.
constexpr std::int64_t kRememberSeconds = 24 * 3600;      // Så länge minns vi att ett Message-ID gett notis.
constexpr std::size_t kMaxItems = 5;                      // Meddelanden som skickas med i notisen.
constexpr std::size_t kMaxSubject = 200;                  // Byte per ämne i notisen.
constexpr int kAttempts = 4;                              // Försök vid tillfälliga fel (429, 5xx, nätverk).
const char* kScope = "https://www.googleapis.com/auth/firebase.messaging";
const char* kDefaultSendUrl = "https://fcm.googleapis.com/v1/projects/{project}/messages:send";

std::int64_t now() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::size_t collect(char* data, std::size_t size, std::size_t n, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * n);
    return size * n;
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

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

std::string displayName(const NewMail& m) {
    return m.fromName.empty() ? m.fromEmail : m.fromName;
}

// Google svarar 404 UNREGISTERED när appen är avinstallerad och 400 när token inte är en FCM-token alls.
bool tokenIsDead(long status, const std::string& body) {
    if (status == 404) return true;
    if (status != 400) return false;
    return body.find("UNREGISTERED") != std::string::npos || body.find("registration token") != std::string::npos;
}

}  // namespace

PushService::PushService(const Config& config, Database& db) : db_(db) {
    if (config.fcmServiceAccountFile.empty()) return;
    std::ifstream in(config.fcmServiceAccountFile, std::ios::binary);
    if (!in) {
        std::cerr << "[push] cannot read " << config.fcmServiceAccountFile << "; push is disabled\n";
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "[push] " << config.fcmServiceAccountFile << " is not valid JSON; push is disabled\n";
        return;
    }
    clientEmail_ = j.value("client_email", "");
    privateKey_ = j.value("private_key", "");
    projectId_ = j.value("project_id", "");
    tokenUrl_ = config.fcmTokenUrl.empty() ? j.value("token_uri", "https://oauth2.googleapis.com/token") : config.fcmTokenUrl;
    if (clientEmail_.empty() || privateKey_.empty() || projectId_.empty()) {
        std::cerr << "[push] the service account file lacks client_email, private_key or project_id; push is disabled\n";
        return;
    }
    sendUrl_ = replaceAll(config.fcmUrl.empty() ? kDefaultSendUrl : config.fcmUrl, "{project}", projectId_);
    configured_ = true;
}

PushService::~PushService() {
    stop();
}

void PushService::start() {
    std::lock_guard lock(mutex_);
    if (started_ || !configured_) return;
    started_ = true;
    thread_ = std::thread(&PushService::loop, this);
}

void PushService::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopping_) return;
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool PushService::stopping() {
    std::lock_guard lock(mutex_);
    return stopping_;
}

void PushService::enqueue(std::vector<NewMail> mails) {
    if (!configured_ || mails.empty()) return;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        for (auto& m : mails) queue_.push_back(std::move(m));
        // En skenande synk ska inte kunna äta upp minnet; det äldsta släpps.
        while (queue_.size() > 10000) queue_.pop_front();
    }
    wake_.notify_all();
}

void PushService::loop() {
    while (true) {
        std::vector<NewMail> batch;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            // Nya mejl kommer ofta flera i taget (en synkomgång, flera mappar); vänta en kort stund och ta allt.
            wake_.wait_for(lock, kCoalesce, [&] { return stopping_; });
            if (stopping_) return;
            batch.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
            queue_.clear();
        }
        try {
            deliver(std::move(batch));
        } catch (const std::exception& e) {
            std::cerr << "[push] delivery failed: " << e.what() << "\n";
        }
    }
}

void PushService::deliver(std::vector<NewMail> batch) {
    std::int64_t t = now();
    // Glöm gamla Message-ID:n.
    for (auto it = notified_.begin(); it != notified_.end();) {
        it = t - it->second > kRememberSeconds ? notified_.erase(it) : std::next(it);
    }
    // Samma meddelande en gång: en kopia som dyker upp i en annan mapp (flytt, AI-sortering) ger ingen ny notis.
    std::vector<NewMail> fresh;
    for (auto& m : batch) {
        if (!m.messageId.empty()) {
            std::string key = std::to_string(m.accountId) + "|" + m.messageId;
            if (notified_.count(key)) continue;
            notified_[key] = t;
        }
        fresh.push_back(std::move(m));
    }
    if (fresh.empty()) return;
    std::stable_sort(fresh.begin(), fresh.end(), [](const NewMail& a, const NewMail& b) { return a.date > b.date; });

    // Per användare och konto: användarens enheter får var sitt meddelande med de mejl de bevakar.
    std::map<long long, std::map<long long, std::vector<const NewMail*>>> byUser;
    for (const auto& m : fresh) byUser[m.userId][m.accountId].push_back(&m);

    for (const auto& [userId, accounts] : byUser) {
        std::vector<Device> devices = db_.listDevices(userId);
        if (devices.empty()) continue;
        for (const auto& device : devices) {
            for (const auto& [accountId, mails] : accounts) {
                if (stopping()) return;
                std::vector<const NewMail*> wanted;
                for (const NewMail* m : mails) {
                    if (wantsFolder(device, *m)) wanted.push_back(m);
                }
                if (wanted.empty()) continue;
                if (!sendToDevice(device, wanted)) break;  // Enheten är borta; resten av kontona är meningslösa.
            }
        }
    }
}

// Enhetens val av mappar: {"<kontoId>": ["INBOX", "Fakturor"]}. Ett konto som saknas i valet ger notiser för
// inkorgen, så att nya konton bevakas utan att appen måste uppdatera valet först.
bool PushService::wantsFolder(const Device& device, const NewMail& mail) {
    if (!device.notify.empty()) {
        json j = json::parse(device.notify, nullptr, false);
        if (j.is_object()) {
            auto it = j.find(std::to_string(mail.accountId));
            if (it != j.end() && it->is_array()) {
                for (const auto& f : *it) {
                    if (f.is_string() && f.get<std::string>() == mail.folder) return true;
                }
                return false;
            }
        }
    }
    return mail.specialUse == "inbox";
}

// Skickar ett datameddelande; appen ritar notisen. Returnerar false om enheten har tagits bort.
bool PushService::sendToDevice(const Device& device, const std::vector<const NewMail*>& mails) {
    const NewMail& newest = *mails.front();
    json items = json::array();
    for (std::size_t i = 0; i < mails.size() && i < kMaxItems; ++i) {
        const NewMail& m = *mails[i];
        items.push_back({{"folder", m.folder},
                         {"uid", m.uid},
                         {"from", displayName(m)},
                         {"subject", textutil::truncateUtf8Bytes(m.subject, kMaxSubject)},
                         {"date", m.date}});
    }
    json message = {
        {"message",
         {{"token", device.token},
          {"android",
           {{"priority", "HIGH"}, {"ttl", "3600s"}, {"collapse_key", "acct-" + std::to_string(newest.accountId)}}},
          {"data",
           {{"type", "new_mail"},
            {"accountId", std::to_string(newest.accountId)},
            {"accountName", newest.accountName},
            {"folder", newest.folder},
            {"uid", std::to_string(newest.uid)},
            // "from" är en reserverad nyckel i FCM:s datameddelanden, därför "sender".
            {"sender", displayName(newest)},
            {"senderEmail", newest.fromEmail},
            {"subject", textutil::truncateUtf8Bytes(newest.subject, kMaxSubject)},
            {"date", std::to_string(newest.date)},
            {"count", std::to_string(mails.size())},
            {"items", items.dump(-1, ' ', false, json::error_handler_t::replace)}}}}},
    };
    std::string body = message.dump(-1, ' ', false, json::error_handler_t::replace);

    bool refreshed = false;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        std::string token;
        try {
            token = accessToken(false);
        } catch (const std::exception& e) {
            std::cerr << "[push] " << e.what() << "\n";
            return true;
        }
        HttpResult r = httpPost(sendUrl_, body, {"Authorization: Bearer " + token, "Content-Type: application/json"});
        if (r.status == 200) {
            std::cout << "[push] notification to device \"" << device.name << "\" (user " << device.userId << "): "
                      << mails.size() << " new in account " << newest.accountId << std::endl;
            return true;
        }
        if (r.status == 401 && !refreshed) {
            // Token har gått ut i förtid eller återkallats: hämta en ny och försök en gång till.
            refreshed = true;
            try {
                accessToken(true);
            } catch (const std::exception& e) {
                std::cerr << "[push] " << e.what() << "\n";
                return true;
            }
            continue;
        }
        if (tokenIsDead(r.status, r.body)) {
            std::cout << "[push] device \"" << device.name << "\" (user " << device.userId
                      << ") is no longer registered; removing it" << std::endl;
            db_.deleteDeviceByToken(device.token);
            return false;
        }
        bool transient = r.status == 0 || r.status == 429 || r.status >= 500;
        if (!transient || attempt + 1 == kAttempts) {
            std::cerr << "[push] FCM responded " << r.status << " for device \"" << device.name << "\": "
                      << (r.error.empty() ? textutil::truncateUtf8Bytes(r.body, 300) : r.error) << "\n";
            return true;
        }
        // 2, 4, 8 sekunder, men sluta direkt vid nedstängning.
        std::unique_lock lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::seconds(2 << attempt), [&] { return stopping_; })) return true;
    }
    return true;
}

std::string PushService::buildJwt(std::int64_t t) const {
    json header = {{"alg", "RS256"}, {"typ", "JWT"}};
    json claims = {{"iss", clientEmail_}, {"scope", kScope}, {"aud", tokenUrl_}, {"iat", t}, {"exp", t + 3600}};
    std::string signingInput = crypto::base64UrlEncode(header.dump()) + "." + crypto::base64UrlEncode(claims.dump());
    return signingInput + "." + crypto::base64UrlEncode(crypto::rs256Sign(privateKey_, signingInput));
}

std::string PushService::accessToken(bool forceRefresh) {
    if (!forceRefresh && !token_.empty() && tokenExpires_ - kTokenMargin > std::chrono::steady_clock::now()) return token_;
    std::string body = "grant_type=" + urlEncode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
                       "&assertion=" + urlEncode(buildJwt(now()));
    HttpResult r = httpPost(tokenUrl_, body, {"Content-Type: application/x-www-form-urlencoded"});
    if (r.status == 0) throw std::runtime_error("could not reach " + tokenUrl_ + ": " + r.error);
    json j = json::parse(r.body, nullptr, false);
    if (r.status != 200 || j.is_discarded() || !j.contains("access_token")) {
        std::string reason = j.is_discarded() ? "invalid response" : j.value("error", "unknown error");
        if (!j.is_discarded() && j.contains("error_description")) reason += ": " + j.value("error_description", "");
        throw std::runtime_error("could not fetch an access token for FCM (" + std::to_string(r.status) + "): " + reason);
    }
    token_ = j.value("access_token", "");
    tokenExpires_ = std::chrono::steady_clock::now() + std::chrono::seconds(j.value("expires_in", 3600));
    return token_;
}

PushService::HttpResult PushService::httpPost(const std::string& url, const std::string& body,
                                              const std::vector<std::string>& headers) {
    HttpResult res;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        res.error = "curl_easy_init failed";
        return res;
    }
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    CURL* c = curl.get();
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    list = curl_slist_append(list, "Expect:");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerList(list, &curl_slist_free_all);

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headerList.get());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        res.status = 0;
        res.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
        return res;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    return res;
}

}  // namespace brevduva
