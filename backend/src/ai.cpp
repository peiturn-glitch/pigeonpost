#include "ai.hpp"

#include "errors.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace brevduva {

namespace {

using json = nlohmann::json;

constexpr long kCompletionTimeout = 180;  // Modeller som resonerar kan behöva någon minut för en omgång.
constexpr long kListTimeout = 30;
constexpr int kAttempts = 2;
constexpr std::size_t kMaxErrorDetail = 300;

std::size_t collectBody(char* data, std::size_t size, std::size_t n, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * n);
    return size * n;
}

bool transient(long status) {
    return status == 0 || status == 500 || status == 502 || status == 503 || status == 504;
}

// Felmeddelandet i tjänstens svar. OpenAI svarar {"error": {"message": ...}}, Gemini ibland med samma objekt i en lista.
std::string errorDetail(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (j.is_array() && !j.empty()) j = j.front();
    std::string detail;
    if (j.is_object() && j.contains("error")) {
        const json& e = j["error"];
        if (e.is_string()) detail = e.get<std::string>();
        else if (e.is_object() && e.contains("message") && e["message"].is_string()) detail = e["message"].get<std::string>();
    }
    if (detail.size() > kMaxErrorDetail) detail = detail.substr(0, kMaxErrorDetail) + "…";
    return detail;
}

// Modellens text. content är oftast en sträng, men kan vara en lista med textdelar.
std::string messageText(const json& response) {
    if (!response.is_object() || !response.contains("choices") || !response["choices"].is_array() ||
        response["choices"].empty()) {
        throw AiError("AI-tjänsten svarade utan innehåll");
    }
    const json& choice = response["choices"].front();
    if (choice.value("finish_reason", "") == "length") {
        throw AiError("AI-tjänstens svar klipptes av. Sortera färre meddelanden åt gången.");
    }
    if (!choice.contains("message") || !choice["message"].is_object()) throw AiError("AI-tjänsten svarade utan innehåll");
    const json& message = choice["message"];
    if (!message.contains("content")) throw AiError("AI-tjänsten svarade utan innehåll");
    const json& content = message["content"];
    if (content.is_string()) return content.get<std::string>();
    std::string text;
    if (content.is_array()) {
        for (const auto& part : content) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) text += part["text"].get<std::string>();
        }
    }
    if (text.empty()) throw AiError("AI-tjänsten svarade utan innehåll");
    return text;
}

// Plockar ut JSON ur svaret även om modellen lagt text eller kodstaket (```json) runt det.
json extractJson(const std::string& text) {
    std::size_t start = text.find_first_of("{[");
    if (start != std::string::npos) {
        std::size_t end = text.find_last_of(text[start] == '{' ? '}' : ']');
        if (end != std::string::npos && end > start) {
            json j = json::parse(text.substr(start, end - start + 1), nullptr, false);
            if (!j.is_discarded()) return j;
        }
    }
    throw AiError("AI-tjänsten svarade inte med giltig JSON");
}

}  // namespace

AiClient::AiClient(std::string baseUrl, std::string apiKey, std::string model)
    : baseUrl_(std::move(baseUrl)), apiKey_(std::move(apiKey)), model_(std::move(model)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

AiClient::Response AiClient::send(const std::string& method, const std::string& path, const std::string& body,
                                  long timeoutSeconds) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init misslyckades");
    Response res;
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    CURL* c = curl.get();

    curl_slist* list = curl_slist_append(nullptr, "Expect:");
    if (!apiKey_.empty()) list = curl_slist_append(list, ("Authorization: Bearer " + apiKey_).c_str());
    if (method == "POST") list = curl_slist_append(list, "Content-Type: application/json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerList(list, &curl_slist_free_all);

    std::string url = baseUrl_ + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    // Adressen kommer från en lista som administratören styr, men följ ändå inga omdirigeringar till andra protokoll.
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headerList.get());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collectBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSeconds);
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

void AiClient::fail(const Response& res) {
    if (res.status == 0) throw AiError("Kunde inte nå AI-tjänsten: " + res.error);
    std::string detail = errorDetail(res.body);
    std::string message;
    // Gemini svarar 400 i stället för 401 på en ogiltig nyckel.
    bool badKey = res.status == 400 && detail.find("API key") != std::string::npos;
    if (res.status == 401 || res.status == 403 || badKey) {
        message = "AI-tjänsten godkände inte API-nyckeln";
    } else if (res.status == 404) {
        message = "AI-tjänsten känner inte till modellen \"" + model_ + "\"";
    } else if (res.status == 429) {
        message = "AI-tjänstens gräns för antal anrop eller kvot är nådd. Försök igen om en stund";
    } else {
        message = "AI-tjänsten svarade med fel " + std::to_string(res.status);
    }
    throw AiError(detail.empty() ? message : message + ": " + detail);
}

json AiClient::completeJson(const std::string& system, const std::string& user) {
    json request = {
        {"model", model_},
        {"messages", json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
        {"response_format", {{"type", "json_object"}}},
    };

    Response res;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(2));
        // Mejltext kan innehålla ogiltig UTF-8; sådana byte ersätts i stället för att ge ett fel.
        res = send("POST", "/chat/completions", request.dump(-1, ' ', false, json::error_handler_t::replace),
                   kCompletionTimeout);
        // Alla tjänster stöder inte response_format. Prompten begär JSON ändå, så försök utan.
        if (res.status == 400 && request.contains("response_format") &&
            res.body.find("response_format") != std::string::npos) {
            request.erase("response_format");
            --attempt;
            continue;
        }
        if (!transient(res.status)) break;
    }
    if (res.status < 200 || res.status >= 300) fail(res);

    json response = json::parse(res.body, nullptr, false);
    if (response.is_discarded()) throw AiError("AI-tjänsten svarade inte med giltig JSON");
    return extractJson(messageText(response));
}

std::vector<std::string> AiClient::listModels() {
    Response res = send("GET", "/models", {}, kListTimeout);
    if (res.status < 200 || res.status >= 300) {
        if (res.status == 404) throw AiError("Adressen svarar inte som ett OpenAI-kompatibelt API (hittar inte /models)");
        fail(res);
    }
    json response = json::parse(res.body, nullptr, false);
    std::vector<std::string> models;
    if (response.is_object() && response.contains("data") && response["data"].is_array()) {
        for (const auto& m : response["data"]) {
            if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) continue;
            std::string id = m["id"].get<std::string>();
            // Gemini listar modeller som "models/gemini-…" men tar emot namnet utan prefix.
            if (id.rfind("models/", 0) == 0) id = id.substr(7);
            models.push_back(std::move(id));
        }
    }
    std::sort(models.begin(), models.end());
    return models;
}

}  // namespace brevduva
