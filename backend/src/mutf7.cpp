#include "mutf7.hpp"

#include <cstdint>
#include <vector>

namespace tbk {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

int alphabetIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == ',') return 63;
    return -1;
}

// Avkodar en UTF-8-sekvens till kodpunkter. Ogiltiga byte blir U+FFFD.
std::vector<char32_t> decodeUtf8(const std::string& s, std::size_t& i) {
    std::vector<char32_t> out;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c <= 0x7e) break;  // Tillbaka till direkt kodning.
        char32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
        else { cp = 0xfffd; extra = 0; }
        ++i;
        for (int k = 0; k < extra; ++k) {
            if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) {
                cp = 0xfffd;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3f);
            ++i;
        }
        out.push_back(cp);
    }
    return out;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

}  // namespace

std::string utf8ToMutf7(const std::string& utf8) {
    std::string out;
    std::size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c >= 0x20 && c <= 0x7e) {
            out += static_cast<char>(c);
            if (c == '&') out += '-';
            ++i;
            continue;
        }

        std::vector<std::uint16_t> units;
        for (char32_t cp : decodeUtf8(utf8, i)) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                units.push_back(static_cast<std::uint16_t>(0xd800 + (cp >> 10)));
                units.push_back(static_cast<std::uint16_t>(0xdc00 + (cp & 0x3ff)));
            } else {
                units.push_back(static_cast<std::uint16_t>(cp));
            }
        }

        out += '&';
        std::uint32_t bits = 0;
        int bitCount = 0;
        for (std::uint16_t u : units) {
            bits = (bits << 16) | u;
            bitCount += 16;
            while (bitCount >= 6) {
                bitCount -= 6;
                out += kAlphabet[(bits >> bitCount) & 0x3f];
            }
        }
        if (bitCount > 0) out += kAlphabet[(bits << (6 - bitCount)) & 0x3f];
        out += '-';
    }
    return out;
}

std::string mutf7ToUtf8(const std::string& mutf7) {
    std::string out;
    std::size_t i = 0;
    while (i < mutf7.size()) {
        char c = mutf7[i];
        if (c != '&') {
            out += c;
            ++i;
            continue;
        }
        ++i;
        if (i < mutf7.size() && mutf7[i] == '-') {
            out += '&';
            ++i;
            continue;
        }

        std::uint32_t bits = 0;
        int bitCount = 0;
        std::vector<std::uint16_t> units;
        while (i < mutf7.size() && mutf7[i] != '-') {
            int v = alphabetIndex(mutf7[i]);
            ++i;
            if (v < 0) continue;
            bits = (bits << 6) | static_cast<std::uint32_t>(v);
            bitCount += 6;
            if (bitCount >= 16) {
                bitCount -= 16;
                units.push_back(static_cast<std::uint16_t>((bits >> bitCount) & 0xffff));
            }
        }
        if (i < mutf7.size()) ++i;  // Hoppa över '-'.

        for (std::size_t k = 0; k < units.size(); ++k) {
            char32_t cp = units[k];
            if (cp >= 0xd800 && cp < 0xdc00 && k + 1 < units.size()) {
                char32_t low = units[k + 1];
                if (low >= 0xdc00 && low < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    ++k;
                }
            }
            appendUtf8(out, cp);
        }
    }
    return out;
}

}  // namespace tbk
