#pragma once

#include "config.hpp"
#include "db.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brevduva {

// Ett nytt meddelande som mejlcachen har upptäckt och som kan ge en notis.
struct NewMail {
    long long userId = 0;
    long long accountId = 0;
    std::string accountName;
    std::string folder;
    std::string specialUse;
    std::uint32_t uid = 0;
    std::string fromName;
    std::string fromEmail;
    std::string subject;
    std::int64_t date = 0;
    std::string messageId;  // Utan vinkelparenteser. Tom om huvudet saknas.
};

// Push-notiser till Android-appen via Firebase Cloud Messaging (HTTP v1). Mejlcachens synktrådar lämnar nya
// meddelanden i kön; en egen tråd samlar ihop dem en kort stund, slår upp användarens enheter och skickar ett
// datameddelande per enhet och konto. Appen ritar själv notisen och hämtar sedan ändringarna.
//
// Inloggningen mot Google sker med ett tjänstekonto (FCM_SERVICE_ACCOUNT_FILE): en JWT signerad med kontots
// RSA-nyckel byts mot en access-token som cachas tills den går ut.
class PushService {
public:
    PushService(const Config& config, Database& db);
    ~PushService();
    PushService(const PushService&) = delete;
    PushService& operator=(const PushService&) = delete;

    // Sant när tjänstekontot kunde läsas. Annars gör enqueue ingenting.
    bool configured() const { return configured_; }
    const std::string& projectId() const { return projectId_; }

    void start();
    void stop();

    // Lämnar nya meddelanden till utskick. Får anropas från vilken tråd som helst och blockerar inte.
    void enqueue(std::vector<NewMail> mails);

private:
    struct HttpResult {
        long status = 0;
        std::string body;
        std::string error;  // Transportfel (curl), då är status 0.
    };

    void loop();
    void deliver(std::vector<NewMail> batch);
    bool sendToDevice(const Device& device, const std::vector<const NewMail*>& mails);
    std::string accessToken(bool forceRefresh);
    std::string buildJwt(std::int64_t now) const;
    static HttpResult httpPost(const std::string& url, const std::string& body, const std::vector<std::string>& headers);
    static bool wantsFolder(const Device& device, const NewMail& mail);
    bool stopping();

    Database& db_;
    bool configured_ = false;
    std::string clientEmail_;
    std::string privateKey_;
    std::string projectId_;
    std::string tokenUrl_;
    std::string sendUrl_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<NewMail> queue_;
    bool started_ = false;
    bool stopping_ = false;
    std::thread thread_;

    // Access-token för FCM, giltig till tokenExpires_. Bara push-tråden använder den.
    std::string token_;
    std::chrono::steady_clock::time_point tokenExpires_{};
    // Meddelanden som redan gett notis (konto|Message-ID -> tid), så att en kopia som flyttas in i en annan
    // bevakad mapp (t.ex. av AI-sorteringen) inte ger en till.
    std::map<std::string, std::int64_t> notified_;
};

}  // namespace brevduva
