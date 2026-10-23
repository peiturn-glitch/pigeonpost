#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Texthjälp som delas av AI-sorteringen och mejlcachen.
namespace brevduva::textutil {

// Klipper efter högst maxChars tecken utan att dela ett UTF-8-tecken.
std::string truncateUtf8(const std::string& s, std::size_t maxChars);
// Som truncateUtf8, men räknat i byte.
std::string truncateUtf8Bytes(const std::string& s, std::size_t maxBytes);
// Blanktecken och radbrytningar blir enkla mellanslag.
std::string collapseWhitespace(const std::string& s);
// Grov omvandling av HTML till text. Det räcker för sökning och för utdrag som en AI ska läsa.
std::string htmlToText(std::string html);

/** Gör texten ofarlig att lägga in i HTML (& < > " ').*/
std::string htmlEscape(const std::string& s);
// Tolkar en söksträng: mellanslag skiljer ord som alla ska finnas med, | skiljer alternativ och citattecken håller
// ihop en fras. `kvitto|faktura "anna svensson"` blir [["kvitto", "faktura"], ["anna svensson"]].
std::vector<std::vector<std::string>> parseSearchTerms(const std::string& query);
// Motsatsen till parseSearchTerms, för sökord som en AI har föreslagit.
std::string formatSearchTerms(const std::vector<std::vector<std::string>>& terms);
// Ersätter ogiltiga UTF-8-sekvenser och NUL-tecken, som databasen annars avvisar.
std::string utf8Clean(const std::string& s);

}  // namespace brevduva::textutil
