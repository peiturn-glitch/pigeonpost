#pragma once

#include "config.hpp"
#include "db.hpp"
#include "mail_service.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace brevduva {

// En AI-tjänst som användare får välja. Listan är fast, så att servern inte kan fås att anropa godtyckliga adresser.
struct AiProvider {
    std::string name;
    std::string baseUrl;
    std::string defaultModel;
    bool keyRequired = true;
};

// Föreslår målmappar för meddelanden med hjälp av en AI-tjänst. Vid manuell sortering flyttas inget här; klienten
// visar förslagen och flyttar det användaren godkänner med det vanliga flyttanropet.
//
// Automatisk sortering körs av en egen tråd för konton där användaren har slagit på den. Bara post som kommer till
// inkorgen efter det sorteras, aldrig till papperskorgen, och varje flytt sparas i en historik och går att ångra.
class AiSorter {
public:
    static constexpr std::size_t kMaxBatch = 25;       // Meddelanden per anrop till AI-tjänsten.
    static constexpr std::size_t kSnippetChars = 500;  // Tecken av meddelandets text som skickas med.
    static constexpr std::size_t kMaxExcluded = 5000;  // Bortvalda mappar som sparas per konto.

    AiSorter(const Config& config, Database& db, MailService& mail);
    ~AiSorter();
    AiSorter(const AiSorter&) = delete;
    AiSorter& operator=(const AiSorter&) = delete;

    void start();
    void stop();

    // Tillgängliga tjänster och användarens inställningar (utan API-nyckeln).
    json state(long long userId);
    json save(long long userId, const json& body);
    void remove(long long userId);
    // Testar inställningarna i body utan att spara dem. Returnerar {ok, error, models}.
    json test(long long userId, const json& body);
    // body: {folder, uids, targets?}. Returnerar {suggestions: [{uid, target, reason}]}, där target är null
    // när meddelandet bör ligga kvar.
    json suggest(long long userId, long long accountId, const json& body);

    // Mappar som AI:n inte får föreslå vid manuell sortering. body: {excluded: [sökvägar]}. Kontot ska redan vara
    // kontrollerat mot användaren. Det som sparats följer med i state().
    json saveSortExcluded(long long userId, long long accountId, const json& body);

    // Hur långt bakåt en mapp har sorterats. body: {folder, beforeUid, date}; nästa omgång tar meddelanden med lägre
    // UID än beforeUid. beforeUid 0 glömmer mappen, så att sorteringen börjar om från de senaste.
    json saveSortCursor(long long userId, long long accountId, const json& body);

    // Gör om en fråga i fritext till sökfilter för /api/search. body: {question, today, accountId?}. Bara frågan,
    // dagens datum och kontonas adresser och mappnamn skickas till AI-tjänsten; inga meddelanden.
    // Returnerar {q, from, to, after, before, unread, flagged, attachments, accountId, folder, explanation}, där
    // after och before är datum (ÅÅÅÅ-MM-DD, till och med) eller tomma.
    json interpretSearch(long long userId, const json& body);

    // Sortering av en hel mapp i bakgrunden, utan granskning. Samma försiktighet som den automatiska sorteringen: bara
    // det AI:n är säker på flyttas, aldrig till papperskorgen, och varje flytt sparas i historiken och går att ångra.
    // body: {folder, targets, unreadOnly, fromStart}. Utan fromStart fortsätter jobbet där mappen senast sorterades.
    json startJob(long long userId, long long accountId, const json& body);
    json jobsState(long long userId);
    // action: pause, resume, cancel eller undo (flyttar tillbaka allt jobbet har flyttat, också i bakgrunden).
    json jobAction(long long userId, long long jobId, const std::string& action);

    // Det sorteringen har lärt sig av användaren: fasta avsändarregler, förslag på nya regler och rättningar.
    json learningState(long long userId);
    // Beslut från granskningen. body: {folder, decisions: [{email, name, subject, suggested, chosen}]}, där tom mapp
    // betyder att meddelandet ligger kvar. Rättningar blir exempel för AI:n, och upprepade beslut blir regelförslag.
    json feedback(long long userId, long long accountId, const json& body);
    // body: {sender, target, dismissed?}. sender är en adress eller en domän ("@exempel.se"); tom target = ligger kvar.
    json saveSenderRule(long long userId, long long accountId, const json& body);
    json deleteSenderRule(long long userId, long long ruleId);
    json deleteExample(long long userId, long long exampleId);
    json clearExamples(long long userId);

    // Regler för automatisk sortering och historiken över flyttade meddelanden.
    json autoState(long long userId);
    // body: {enabled, targets, intervalMinutes}. Kontot ska redan vara kontrollerat mot användaren.
    json saveAutoRule(long long userId, long long accountId, const json& body);
    // Flyttar tillbaka ett automatiskt sorterat meddelande. Det sorteras inte igen.
    json undo(long long userId, long long logId);
    // En mapp har bytt namn eller flyttats: regeln för automatisk sortering ska peka på den nya sökvägen.
    void folderRenamed(long long accountId, const std::string& from, const std::string& to);

private:
    struct Target {
        std::string path;
        std::string role;
    };
    struct Suggestion {
        std::uint32_t uid = 0;
        std::string target;  // Tom = ligger kvar.
        std::string reason;
    };

    std::vector<Target> targetFolders(long long accountId, const std::string& source,
                                      const std::optional<std::set<std::string>>& wanted, bool allowTrash);
    std::vector<Suggestion> askModel(const AiSettings& settings, const std::string& folder,
                                     const std::vector<Target>& targets, const std::vector<MessageSummary>& summaries,
                                     bool unattended, const json& corrections);
    // Förslag för varje meddelande, i samma ordning som summaries. Användarens avsändarregler går först och kostar
    // inget AI-anrop; resten frågas AI:n om, med användarens tidigare rättningar som exempel.
    std::vector<Suggestion> classify(const AiSettings& settings, long long accountId, const std::string& folder,
                                     const std::vector<Target>& targets, const std::vector<MessageSummary>& summaries,
                                     bool unattended);
    json pickCorrections(long long accountId, const std::string& folder, const std::vector<Target>& targets,
                         const std::vector<const MessageSummary*>& messages);
    void loop();
    void runRule(AiAutoRule rule);
    void sortNewMessages(AiAutoRule& rule);
    void runJob(AiSortJob job);
    void sortJobBatch(AiSortJob& job);
    void undoJobBatch(AiSortJob& job);
    // Flyttar märket för manuell sortering bakåt, aldrig framåt.
    void lowerSortCursor(long long userId, long long accountId, const std::string& folder, std::uint32_t uid,
                         std::int64_t date);
    void wakeWorker();

    const AiProvider* provider(const std::string& baseUrl) const;
    AiSettings settingsFromJson(long long userId, const json& body);

    Database& db_;
    MailService& mail_;
    std::vector<AiProvider> providers_;

    std::mutex prefsMutex_;  // Skyddar läs-ändra-spara av det sparade valet vid manuell sortering.
    std::mutex mutex_;
    std::condition_variable wake_;
    bool started_ = false;
    bool stopping_ = false;
    bool kick_ = false;  // En regel har sparats; kontrollera utan att vänta.
    bool jobWait_ = false;  // Ett bakgrundsjobb har mer att göra; vänta bara dess paus.
    std::thread thread_;
};

}  // namespace brevduva
