#pragma once

#include <string>

namespace brevduva::crypto {

std::string randomBytes(std::size_t n);
std::string toHex(const std::string& bytes);
std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);
// Base64url utan utfyllnad (JWT).
std::string base64UrlEncode(const std::string& bytes);
std::string sha256(const std::string& data);
// RSASSA-PKCS1-v1_5 med SHA-256 (JWT RS256). pemPrivateKey är en PEM-kodad privat nyckel.
std::string rs256Sign(const std::string& pemPrivateKey, const std::string& data);
bool constantTimeEquals(const std::string& a, const std::string& b);

// Lösenordshash med scrypt. Format: scrypt$N$r$p$salt$hash (base64).
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& encoded);

// AES-256-GCM. Utdata: base64(iv || tag || chiffertext).
std::string encrypt(const std::string& key32, const std::string& plaintext);
std::string decrypt(const std::string& key32, const std::string& token);

}  // namespace brevduva::crypto
