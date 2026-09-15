#pragma once

#include <string>

namespace tbk::crypto {

std::string randomBytes(std::size_t n);
std::string toHex(const std::string& bytes);
std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);
std::string sha256(const std::string& data);
bool constantTimeEquals(const std::string& a, const std::string& b);

// Lösenordshash med scrypt. Format: scrypt$N$r$p$salt$hash (base64).
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& encoded);

// AES-256-GCM. Utdata: base64(iv || tag || chiffertext).
std::string encrypt(const std::string& key32, const std::string& plaintext);
std::string decrypt(const std::string& key32, const std::string& token);

}  // namespace tbk::crypto
