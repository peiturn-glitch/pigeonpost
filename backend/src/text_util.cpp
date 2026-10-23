#include "text_util.hpp"

#include <cctype>
#include <map>

namespace brevduva::textutil {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string truncateUtf8(const std::string& s, std::size_t maxChars) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && chars++ == maxChars) break;
        ++i;
    }
    return i < s.size() ? s.substr(0, i) + "…" : s;
}

std::string collapseWhitespace(const std::string& s) {
    std::string out;
    bool space = false;
    for (unsigned char c : s) {
        if (std::isspace(c) || c < 0x20) {
            space = !out.empty();
        } else {
            if (space) out += ' ';
            space = false;
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string utf8FromCodepoint(unsigned long cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// Avkodar numeriska HTML-entiteter och de vanligaste namngivna. Okända lämnas som de är.
std::string decodeEntities(const std::string& text) {
    static const std::map<std::string, std::string> named = {
        {"nbsp", " "}, {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"zwnj", ""},
        {"aring", "å"}, {"auml", "ä"}, {"ouml", "ö"}, {"Aring", "Å"}, {"Auml", "Ä"}, {"Ouml", "Ö"},
        {"eacute", "é"}, {"uuml", "ü"}, {"ndash", "–"}, {"mdash", "—"}, {"hellip", "…"}, {"copy", "©"}, {"euro", "€"}};
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::size_t end = text[i] == '&' ? text.find(';', i) : std::string::npos;
        if (end == std::string::npos || end - i > 10 || end - i < 2) {
            out += text[i];
            continue;
        }
        std::string name = text.substr(i + 1, end - i - 1);
        std::string decoded;
        bool known = false;
        if (name[0] == '#') {
            try {
                bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
                unsigned long cp = std::stoul(name.substr(hex ? 2 : 1), nullptr, hex ? 16 : 10);
                decoded = utf8FromCodepoint(cp < 0x20 ? ' ' : cp);
                known = !decoded.empty();
            } catch (const std::exception&) {
            }
        } else if (auto it = named.find(name); it != named.end()) {
            decoded = it->second;
            known = true;
        }
        if (!known) {
            out += text[i];
            continue;
        }
        out += decoded;
        i = end;
    }
    return out;
}

// Tar bort ett element med innehåll, t.ex. <style>…</style>. Saknas sluttaggen (avklippt text) tas resten bort.
void removeElement(std::string& html, const std::string& tag) {
    std::string lowered = lower(html);
    std::size_t pos = 0;
    while ((pos = lowered.find("<" + tag, pos)) != std::string::npos) {
        // "<head" ska inte träffa "<header".
        std::size_t after = pos + tag.size() + 1;
        if (after < lowered.size() && std::isalnum(static_cast<unsigned char>(lowered[after]))) {
            pos = after;
            continue;
        }
        std::size_t end = lowered.find("</" + tag, pos);
        std::size_t close = end == std::string::npos ? std::string::npos : lowered.find('>', end);
        std::size_t length = close == std::string::npos ? std::string::npos : close + 1 - pos;
        html.erase(pos, length);
        lowered.erase(pos, length);
    }
}

// Grov omvandling av HTML till text. Det räcker för ett utdrag som en AI ska läsa.
std::string htmlToText(std::string html) {
    removeElement(html, "head");
    removeElement(html, "style");
    removeElement(html, "script");
    std::string text;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') {
            inTag = true;
            text += ' ';
        } else if (c == '>') {
            inTag = false;
        } else if (!inTag) {
            text += c;
        }
    }
    return decodeEntities(text);
}


std::string truncateUtf8Bytes(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end);
}

std::string utf8Clean(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool valid = length > 0 && i + length <= s.size() && c != 0;
        for (std::size_t k = 1; valid && k < length; ++k) {
            valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        }
        // Överlånga former och surrogat godtas inte heller av MariaDB.
        if (valid && length == 2 && c < 0xC2) valid = false;
        if (valid && length == 3) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) valid = false;
        }
        if (valid && length == 4) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || c > 0xF4 || (c == 0xF4 && c1 >= 0x90)) valid = false;
        }
        if (valid) {
            out.append(s, i, length);
            i += length;
        } else {
            if (c != 0) out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
        }
    }
    return out;
}

std::vector<std::vector<std::string>> parseSearchTerms(const std::string& query) {
    constexpr std::size_t kMaxGroups = 10, kMaxAlternatives = 8, kMaxLength = 100;
    std::vector<std::vector<std::string>> terms;
    std::vector<std::string> group;
    std::string word;
    bool quoted = false;
    auto endWord = [&]() {
        if (!word.empty() && group.size() < kMaxAlternatives) group.push_back(truncateUtf8Bytes(word, kMaxLength));
        word.clear();
    };
    auto endGroup = [&]() {
        endWord();
        if (!group.empty() && terms.size() < kMaxGroups) terms.push_back(std::move(group));
        group.clear();
    };
    for (char c : query) {
        if (c == '"') quoted = !quoted;
        else if (!quoted && c == '|') endWord();
        else if (!quoted && std::isspace(static_cast<unsigned char>(c))) endGroup();
        else word += c;
    }
    endGroup();
    return terms;
}

std::string formatSearchTerms(const std::vector<std::vector<std::string>>& terms) {
    std::string out;
    for (const auto& group : terms) {
        std::string text;
        for (const auto& alternative : group) {
            std::string word;
            for (char c : collapseWhitespace(alternative)) {
                if (c != '"' && c != '|') word += c;
            }
            if (word.empty()) continue;
            if (word.find(' ') != std::string::npos) word = '"' + word + '"';
            text += (text.empty() ? "" : "|") + word;
        }
        if (!text.empty()) out += (out.empty() ? "" : " ") + text;
    }
    return out;
}


std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace brevduva::textutil
