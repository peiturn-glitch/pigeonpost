#pragma once

#include "db.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tbk::thunderbird {

using json = nlohmann::json;

// Ett värde i prefs.js: user_pref("namn", värde);
struct PrefValue {
    enum class Type { String, Int, Bool };
    Type type = Type::String;
    std::string str;
    long long num = 0;
    bool boolean = false;
};

using Prefs = std::map<std::string, PrefValue>;

// Tolkar prefs.js (samma syntax som Mozillas prefs-parser: user_pref/pref/sticky_pref, kommentarer,
// strängar med \n \" \\ \xHH \uHHHH). Ogiltiga rader hoppas över.
Prefs parsePrefs(const std::string& text);

// Bygger en förhandsgranskning av IMAP-konton och identiteter i prefs.js.
// existing används för att markera konton som redan finns hos användaren.
json importPreview(const Prefs& prefs, const std::vector<Account>& existing);

// Skriver användarens konton och avsändare som prefs.js i Thunderbirds format (utan lösenord).
std::string exportPrefs(const std::vector<std::pair<Account, std::vector<Identity>>>& accounts);

// En zip-fil (lagrad, okomprimerad) med de angivna filerna.
std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace tbk::thunderbird
