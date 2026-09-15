#include "config.hpp"

#include "crypto.hpp"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace brevduva {

namespace {

std::string env(const char* name, const std::string& fallback = {}) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

// Läser eller skapar en slumpad nyckel i datakatalogen när SECRET_KEY saknas.
std::string loadOrCreateSecret(const std::string& dataDir) {
    std::filesystem::path path = std::filesystem::path(dataDir) / "secret.key";
    if (std::filesystem::exists(path)) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    std::string secret = crypto::toHex(crypto::randomBytes(32));
    {
        std::ofstream out(path, std::ios::binary);
        out << secret;
    }
    ::chmod(path.c_str(), 0600);
    std::cerr << "[config] SECRET_KEY saknas; skapade " << path << "\n";
    return secret;
}

}  // namespace

Config Config::fromEnvironment() {
    Config c;
    c.host = env("BREVDUVA_HOST", c.host);
    c.port = std::stoi(env("BREVDUVA_PORT", std::to_string(c.port)));
    c.dataDir = env("BREVDUVA_DATA_DIR", c.dataDir);
    c.staticDir = env("BREVDUVA_STATIC_DIR", c.staticDir);
    c.adminUsername = env("ADMIN_USERNAME");
    c.adminPassword = env("ADMIN_PASSWORD");
    c.legacyAppPassword = !env("APP_PASSWORD").empty();
    c.publicUrl = env("PUBLIC_URL");
    while (!c.publicUrl.empty() && c.publicUrl.back() == '/') c.publicUrl.pop_back();
    c.googleClientId = env("GOOGLE_CLIENT_ID");
    c.googleClientSecret = env("GOOGLE_CLIENT_SECRET");
    c.googleAuthUrl = env("GOOGLE_AUTH_URL", c.googleAuthUrl);
    c.googleTokenUrl = env("GOOGLE_TOKEN_URL", c.googleTokenUrl);
    c.googleRevokeUrl = env("GOOGLE_REVOKE_URL", c.googleRevokeUrl);

    std::filesystem::create_directories(c.dataDir);
    std::string secret = env("SECRET_KEY");
    if (secret.empty()) secret = loadOrCreateSecret(c.dataDir);
    c.encryptionKey = crypto::sha256(secret);
    return c;
}

}  // namespace brevduva
