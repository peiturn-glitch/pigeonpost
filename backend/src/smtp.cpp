#include "smtp.hpp"

#include "errors.hpp"
#include "imap.hpp"

#include <curl/curl.h>

#include <cstring>
#include <memory>

namespace tbk {

namespace {

struct Upload {
    const std::string* data;
    std::size_t offset = 0;
};

std::size_t readCallback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    auto* up = static_cast<Upload*>(userdata);
    std::size_t room = size * nitems;
    std::size_t left = up->data->size() - up->offset;
    std::size_t n = left < room ? left : room;
    std::memcpy(buffer, up->data->data() + up->offset, n);
    up->offset += n;
    return n;
}

using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

CurlPtr configure(const Account& account, char* errorBuffer) {
    CurlPtr curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init misslyckades");

    Security sec = parseSecurity(account.smtpSecurity);
    std::string url = (sec == Security::Tls ? "smtps://" : "smtp://") + account.smtpHost + ":" +
                      std::to_string(account.smtpPort);
    CURL* c = curl.get();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (sec == Security::StartTls) curl_easy_setopt(c, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, account.tlsVerify ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, account.tlsVerify ? 2L : 0L);

    const std::string& user = account.smtpUsername.empty() ? account.username : account.smtpUsername;
    if (account.usesGoogleOAuth()) {
        // XOAUTH2: access-token hämtas av anroparen (GoogleOAuth) och sätts på kontot.
        if (account.oauthAccessToken.empty()) throw MailServerError("Google-inloggning saknas för SMTP");
        curl_easy_setopt(c, CURLOPT_USERNAME, account.username.c_str());
        curl_easy_setopt(c, CURLOPT_XOAUTH2_BEARER, account.oauthAccessToken.c_str());
        curl_easy_setopt(c, CURLOPT_LOGIN_OPTIONS, "AUTH=XOAUTH2");
        // Token på samma rad som AUTH, som i Googles dokumentation; en del servrar kräver det.
        curl_easy_setopt(c, CURLOPT_SASL_IR, 1L);
    } else if (!user.empty()) {
        curl_easy_setopt(c, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(c, CURLOPT_PASSWORD, account.password.c_str());
    }
    return curl;
}

std::string angle(const std::string& addr) {
    return "<" + addr + ">";
}

}  // namespace

void smtpSend(const Account& account, const std::string& from, const std::vector<std::string>& recipients,
              const std::string& message) {
    if (recipients.empty()) throw BadRequest("Minst en mottagare krävs");

    char errorBuffer[CURL_ERROR_SIZE] = {0};
    CurlPtr curl = configure(account, errorBuffer);
    CURL* c = curl.get();

    std::string mailFrom = angle(from);
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, mailFrom.c_str());

    curl_slist* rcpt = nullptr;
    for (const auto& r : recipients) rcpt = curl_slist_append(rcpt, angle(r).c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> rcptGuard(rcpt, &curl_slist_free_all);
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);

    Upload up{&message};
    curl_easy_setopt(c, CURLOPT_READFUNCTION, readCallback);
    curl_easy_setopt(c, CURLOPT_READDATA, &up);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(message.size()));

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        std::string detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
        throw MailServerError("Kunde inte skicka via SMTP: " + detail);
    }
}

void smtpTest(const Account& account) {
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    CurlPtr curl = configure(account, errorBuffer);
    CURL* c = curl.get();
    // Utan eget kommando skickar libcurl HELP, som inte alla servrar stöder. NOOP är obligatoriskt (RFC 5321).
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "NOOP");
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        std::string detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
        throw MailServerError("SMTP-test misslyckades: " + detail);
    }
}

}  // namespace tbk
