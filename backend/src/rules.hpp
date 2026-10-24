#pragma once

#include "config.hpp"
#include "db.hpp"
#include "mail_service.hpp"
#include "mime.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace brevduva {

using json = nlohmann::json;

class MailCache;

// Det en regel tittar på hos ett meddelande.
struct RuleInput {
    std::vector<mime::Address> from, to;
    std::string subject;
};

// Vad reglerna kom fram till för ett meddelande.
struct RuleOutcome {
    bool matched = false;      // Minst en regel stämde.
    std::string move;          // Mapp att flytta till, eller tom.
    bool read = false, star = false, junk = false, trash = false;
    long long ruleId = 0;      // Regeln som bestämde flytten (eller den första som stämde).
    std::string ruleName;
};

// Regler för inkommande post: villkor på avsändare, mottagare och ämne, och åtgärder som flytt, läst och stjärna.
// Reglerna körs av en egen tråd på ny post i varje kontos inkorg, oberoende av AI-sorteringen, och de kan köras på
// en hel mapp i efterhand. Varje flytt sparas i samma historik som AI-sorteringen och går att ångra.
//
// Förslag på regler tas fram ur mejlcachen: avsändare som nästan alltid ligger i en viss mapp blir ett förslag, och
// med AI-tjänsten påslagen får den se avsändare och ämnesrader per mapp (aldrig innehåll) och föreslå fler. Varje
// förslag provkörs mot cachen, så att användaren ser hur många meddelanden det träffar innan det godkänns.
class RuleEngine {
public:
    static constexpr std::size_t kMaxRules = 200;        // Per konto.
    static constexpr std::size_t kMaxConditions = 10;
    static constexpr std::size_t kMaxPerRun = 50;        // Nya meddelanden per konto och kontroll.
    static constexpr int kHeaderSample = 5000;           // Rubrikrader per mapp vid provkörning och förslag.

    // cache = nullptr när mejlcachen är avstängd; då finns inga förslag och ingen provkörning.
    RuleEngine(const Config& config, Database& db, MailService& mail, MailCache* cache);
    ~RuleEngine();
    RuleEngine(const RuleEngine&) = delete;
    RuleEngine& operator=(const RuleEngine&) = delete;

    void start();
    void stop();
    // Ny post har kommit till kontots inkorg (från mejlcachen): kör reglerna snarast.
    void kick(long long accountId);

    // --- API ---
    json state(long long userId);
    // body: {name, enabled, matchAll, conditions, actions}. Kontot ska redan vara kontrollerat mot användaren.
    json save(long long userId, long long accountId, const json& body);
    json update(long long userId, long long ruleId, const json& body);
    json remove(long long userId, long long ruleId);
    json reorder(long long userId, long long accountId, const json& body);
    // Provkör en regel mot en mapp i cachen. body: {rule, folder}. Returnerar {total, matches, sample}.
    json test(long long userId, long long accountId, const json& body);
    // Kör reglerna på en hel mapp i bakgrunden. body: {folder}. Returnerar state().
    json run(long long userId, long long accountId, const json& body);
    // Förslag på regler ur mapparnas innehåll. body: {useAi}. Returnerar {suggestions, aiUsed}.
    json suggest(long long userId, long long accountId, const json& body);
    // body: {sender}. Ett förslag för avsändaren ska inte visas igen.
    json dismiss(long long userId, long long accountId, const json& body);

    // --- För AI-sorteringen ---
    // Reglernas beslut för varje meddelande, i samma ordning. Ett meddelande som ingen regel stämmer på får matched = false.
    std::vector<RuleOutcome> evaluate(long long accountId, const std::string& folder,
                                      const std::vector<MessageSummary>& messages);
    // En mapp har bytt namn eller flyttats: regler som flyttar dit ska peka på den nya sökvägen.
    void folderRenamed(long long accountId, const std::string& from, const std::string& to);

    // Är avsändaren (adress eller domän) redan täckt av en regel på avsändaren?
    static bool coversSender(const std::vector<MailRule>& rules, const std::string& sender);
    static RuleInput inputOf(const MessageSummary& m);
    static RuleInput inputOf(const CacheMessage& m);
    static bool matches(const MailRule& rule, const RuleInput& input);
    // Kontrollerar och normaliserar en regel från klienten. Kastar BadRequest.
    MailRule validate(long long userId, long long accountId, const json& body);

private:
    struct Job {
        long long userId = 0;
        long long accountId = 0;
        std::string folder;
        std::string state = "queued";  // queued, running, done, failed
        std::int64_t total = 0;
        std::int64_t processed = 0;
        std::int64_t moved = 0;
        std::int64_t changed = 0;  // Flaggor ändrade utan flytt.
        std::string error;
        std::int64_t startedAt = 0;
        std::int64_t finishedAt = 0;
    };
    struct FolderInfo {
        std::string path;
        std::string specialUse;
    };

    void loop();
    void checkAccount(RuleState state);
    void applyNew(RuleState& state);
    void runJob(Job& job);
    // Kör reglerna på meddelandena och gör det som ska göras. Returnerar (flyttade, flaggade).
    std::pair<int, int> apply(long long userId, long long accountId, const std::string& folder,
                              const std::vector<MessageSummary>& messages, const std::vector<MailRule>& rules,
                              const std::vector<FolderInfo>& folders, bool skipSorted);
    std::vector<FolderInfo> folderList(long long accountId);
    static RuleOutcome decide(const std::vector<MailRule>& rules, const RuleInput& input, const std::string& folder);
    static std::string resolveTarget(const RuleOutcome& outcome, const std::vector<FolderInfo>& folders);
    json ruleJson(const MailRule& rule);
    json historicalSuggestions(long long userId, long long accountId, const std::vector<MailRule>& existing);
    json aiSuggestions(long long userId, long long accountId, const std::vector<MailRule>& existing, const json& historical);
    // Träffar för en regel i kontots cachade mappar: {folder: antal}.
    std::map<std::string, std::int64_t> hitCounts(long long accountId, const MailRule& rule, int maxFolders);

    const Config& config_;
    Database& db_;
    MailService& mail_;
    MailCache* cache_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool started_ = false;
    bool stopping_ = false;
    bool kicked_ = false;
    std::deque<Job> jobs_;      // Kör nu-jobb, det första pågår.
    std::vector<Job> finished_;  // De senaste avslutade jobben, för state().
    std::thread thread_;
};

}  // namespace brevduva
