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

    // Anslutning till MariaDB.
    std::string dbHost = "localhost";
    int dbPort = 3306;
    std::string dbName = "brevduva";
    std::string dbUser = "brevduva";
    std::string dbPassword;

    // Adressen som webbläsaren når appen på, t.ex. https://mail.exempel.se. Används för OAuth-callback.
    std::string publicUrl;
    // Den som driver instansen, för integritetspolicyn och användarvillkoren (/privacy.html, /terms.html).
    std::string operatorName = "the operator of this service";
    std::string operatorContact;  // e-postadress eller webbadress; tom = "contact the operator"

    // Google-inloggning för Gmail (OAuth-klient av typen "Webbapplikation" i Google Cloud).
    std::string googleClientId;
    std::string googleClientSecret;
    // Endpoints kan bytas ut, t.ex. mot en testserver.
    std::string googleAuthUrl = "https://accounts.google.com/o/oauth2/v2/auth";
    std::string googleTokenUrl = "https://oauth2.googleapis.com/token";
    std::string googleRevokeUrl = "https://oauth2.googleapis.com/revoke";
    // Bas för Google Drive API (backup till Google Drive).
    std::string googleApiUrl = "https://www.googleapis.com";
    // Extra AI-tjänster för sortering utöver de inbyggda, som "Namn=https://adress/v1" åtskilda med komma.
    std::string aiExtraProviders;
    // Mejlcache i databasen (MAIL_CACHE=0 stänger av den; då läses allt direkt från mejlservern som förr).
    bool mailCache = true;
    // Hur ofta papperskorgarna gås igenom för tömning (sekunder). TRASH_DAY_SECONDS låter testerna räkna
    // "dagar" i sekunder i stället för 86 400.
    int trashPurgeInterval = 15 * 60;
    int trashDaySeconds = 24 * 3600;
    int ruleCheckSeconds = 5 * 60;  // Hur ofta reglerna letar efter ny post utan mejlcachens hjälp.
    // Hela meddelanden som öppnats nyligen sparas som filer, så att bilagor inte hämtas om. 0 stänger av.
    std::string rawCacheDir;
    int rawCacheMb = 1024;
    // Backupfiler (zip) som väntar på nedladdning eller uppladdning.
    std::string backupDir;
    // Push-notiser till Android-appen via Firebase Cloud Messaging: JSON-nyckeln för ett tjänstekonto i
    // Firebase-projektet. Tom = inga notiser. Adresserna kan bytas ut mot en testserver; {project} ersätts
    // med projektets id.
    std::string fcmServiceAccountFile;
    std::string fcmUrl = "https://fcm.googleapis.com/v1/projects/{project}/messages:send";
    std::string fcmTokenUrl;  // Tom = token_uri ur nyckelfilen.
    // APK och version.json för Android-appen serveras under /android från den här katalogen.
    std::string androidDir;
    std::string encryptionKey;  // 32 byte, härledd från SECRET_KEY eller /data/secret.key.

    static Config fromEnvironment();
};

}  // namespace brevduva
