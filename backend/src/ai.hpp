#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace brevduva {

// En liten klient för AI-tjänster med OpenAI-kompatibelt API (Gemini, OpenAI, Mistral, Ollama med flera).
class AiClient {
public:
    // apiKey får vara tom för tjänster utan inloggning, till exempel en lokal Ollama.
    AiClient(std::string baseUrl, std::string apiKey, std::string model);

    // Skickar en systemprompt och ett användarmeddelande och returnerar modellens svar tolkat som JSON.
    nlohmann::json completeJson(const std::string& system, const std::string& user);

    // Modeller som nyckeln har tillgång till. Används också för att testa anslutningen.
    std::vector<std::string> listModels();

private:
    struct Response {
        long status = 0;  // 0 = nätverksfel
        std::string body;
        std::string error;
    };

    Response send(const std::string& method, const std::string& path, const std::string& body, long timeoutSeconds);
    [[noreturn]] void fail(const Response& res);

    std::string baseUrl_;
    std::string apiKey_;
    std::string model_;
};

}  // namespace brevduva
