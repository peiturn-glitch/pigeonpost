#include "crypto.hpp"

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace brevduva::crypto {

namespace {

constexpr int kIvLen = 12;
constexpr int kTagLen = 16;

using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

CipherCtx newCtx() {
    CipherCtx ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    return ctx;
}

const unsigned char* u(const std::string& s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}

}  // namespace

std::string randomBytes(std::size_t n) {
    std::string out(n, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return out;
}

std::string toHex(const std::string& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 0x0f];
    }
    return out;
}

std::string base64Encode(const std::string& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), u(bytes),
                            static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string base64Decode(const std::string& text) {
    if (text.size() % 4 != 0) throw std::runtime_error("Invalid base64");
    std::string out(3 * text.size() / 4, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()), u(text),
                            static_cast<int>(text.size()));
    if (n < 0) throw std::runtime_error("Invalid base64");
    // EVP_DecodeBlock räknar utfyllnad som data; ta bort motsvarande nollor.
    std::size_t padding = 0;
    if (!text.empty() && text.back() == '=') ++padding;
    if (text.size() > 1 && text[text.size() - 2] == '=') ++padding;
    out.resize(static_cast<std::size_t>(n) - padding);
    return out;
}

std::string base64UrlEncode(const std::string& bytes) {
    std::string s = base64Encode(bytes);
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string rs256Sign(const std::string& pemPrivateKey, const std::string& data) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pemPrivateKey.data(), static_cast<int>(pemPrivateKey.size())), &BIO_free);
    if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
                                                            &EVP_PKEY_free);
    if (!key) throw std::runtime_error("Could not read the private key (PEM)");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        throw std::runtime_error("EVP_DigestSignInit failed");
    }
    const auto* in = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t len = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &len, in, data.size()) != 1) throw std::runtime_error("EVP_DigestSign failed");
    std::string sig(len, '\0');
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(sig.data()), &len, in, data.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign failed");
    }
    sig.resize(len);
    return sig;
}

std::string sha256(const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA-256 failed");
    }
    return std::string(reinterpret_cast<char*>(md), len);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    // Jämför hashar så att längden inte läcker.
    std::string ha = sha256(a);
    std::string hb = sha256(b);
    return CRYPTO_memcmp(ha.data(), hb.data(), ha.size()) == 0;
}

namespace {

constexpr std::uint64_t kScryptN = 1 << 15;
constexpr std::uint64_t kScryptR = 8;
constexpr std::uint64_t kScryptP = 1;
constexpr std::size_t kScryptKeyLen = 32;
constexpr std::uint64_t kScryptMaxMem = 128 * 1024 * 1024;

std::string scrypt(const std::string& password, const std::string& salt, std::uint64_t n, std::uint64_t r,
                   std::uint64_t p) {
    std::string key(kScryptKeyLen, '\0');
    if (EVP_PBE_scrypt(password.data(), password.size(), u(salt), salt.size(), n, r, p, kScryptMaxMem,
                       reinterpret_cast<unsigned char*>(key.data()), key.size()) != 1) {
        throw std::runtime_error("scrypt failed");
    }
    return key;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find(sep, start);
        parts.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

}  // namespace

std::string hashPassword(const std::string& password) {
    std::string salt = randomBytes(16);
    std::string key = scrypt(password, salt, kScryptN, kScryptR, kScryptP);
    return "scrypt$" + std::to_string(kScryptN) + "$" + std::to_string(kScryptR) + "$" + std::to_string(kScryptP) +
           "$" + base64Encode(salt) + "$" + base64Encode(key);
}

bool verifyPassword(const std::string& password, const std::string& encoded) {
    auto parts = split(encoded, '$');
    if (parts.size() != 6 || parts[0] != "scrypt") return false;
    try {
        std::uint64_t n = std::stoull(parts[1]);
        std::uint64_t r = std::stoull(parts[2]);
        std::uint64_t p = std::stoull(parts[3]);
        std::string expected = base64Decode(parts[5]);
        std::string actual = scrypt(password, base64Decode(parts[4]), n, r, p);
        return expected.size() == actual.size() && CRYPTO_memcmp(expected.data(), actual.data(), actual.size()) == 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::string encrypt(const std::string& key32, const std::string& plaintext) {
    std::string iv = randomBytes(kIvLen);
    auto ctx = newCtx();
    std::string cipher(plaintext.size(), '\0');
    int len = 0;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, u(key32), u(iv)) != 1 ||
        EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(cipher.data()), &len,
                          u(plaintext), static_cast<int>(plaintext.size())) != 1) {
        throw std::runtime_error("Encryption failed");
    }
    int finalLen = 0;
    unsigned char tag[kTagLen];
    if (EVP_EncryptFinal_ex(ctx.get(), nullptr, &finalLen) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagLen, tag) != 1) {
        throw std::runtime_error("Encryption failed");
    }
    return base64Encode(iv + std::string(reinterpret_cast<char*>(tag), kTagLen) + cipher);
}

std::string decrypt(const std::string& key32, const std::string& token) {
    std::string raw = base64Decode(token);
    if (raw.size() < kIvLen + kTagLen) throw std::runtime_error("Invalid ciphertext");
    std::string iv = raw.substr(0, kIvLen);
    std::string tag = raw.substr(kIvLen, kTagLen);
    std::string cipher = raw.substr(kIvLen + kTagLen);

    auto ctx = newCtx();
    std::string plain(cipher.size(), '\0');
    int len = 0;
    int finalLen = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, u(key32), u(iv)) != 1 ||
        EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &len,
                          u(cipher), static_cast<int>(cipher.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagLen,
                            const_cast<char*>(tag.data())) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), nullptr, &finalLen) != 1) {
        throw std::runtime_error("Decryption failed (wrong SECRET_KEY?)");
    }
    return plain;
}

}  // namespace brevduva::crypto
