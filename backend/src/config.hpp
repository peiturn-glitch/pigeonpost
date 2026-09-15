#pragma once

#include <string>

namespace brevduva {

struct Config {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string dataDir = "/data";
    std::string staticDir = "/app/public";
    // Skapar en första administratör vid start om databasen saknar användare.
    std::string adminUsername;
    std::string adminPassword;
    bool legacyAppPassword = false;  // APP_PASSWORD från enanvändarversionen är satt.

    // Adressen som webbläsaren når appen på, t.ex. https://mail.exempel.se. Används för OAuth-callback.
    std::string publicUrl;

    // Google-inloggning för Gmail (OAuth-klient av typen "Webbapplikation" i Google Cloud).
    std::string googleClientId;
    std::string googleClientSecret;
    // Endpoints kan bytas ut, t.ex. mot en testserver.
    std::string googleAuthUrl = "https://accounts.google.com/o/oauth2/v2/auth";
    std::string googleTokenUrl = "https://oauth2.googleapis.com/token";
    std::string googleRevokeUrl = "https://oauth2.googleapis.com/revoke";
    std::string encryptionKey;  // 32 byte, härledd från SECRET_KEY eller /data/secret.key.

    static Config fromEnvironment();
};

}  // namespace brevduva
