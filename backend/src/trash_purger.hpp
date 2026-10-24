#pragma once

#include "config.hpp"
#include "db.hpp"
#include "mail_service.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace brevduva {

class MailCache;

// Tömmer papperskorgarna: meddelanden som legat där längre än users.trash_days dagar tas bort permanent
// (0 = aldrig). Tiden räknas från när rensaren först såg meddelandet i papperskorgen (tabellen trash_seen),
// eftersom IMAP inte berättar när ett meddelande flyttades dit - MOVE behåller det ursprungliga datumet.
// Första passet efter att funktionen slagits på registrerar därför bara vad som finns, och ingenting tas
// bort förrän antalet dagar har gått. Varje konto gås igenom med en egen IMAP-anslutning i en egen tråd,
// så webbgränssnittet väntar aldrig på rensningen.
class TrashPurger {
public:
    // cache = nullptr när mejlcachen är avstängd.
    TrashPurger(const Config& config, Database& db, MailService& mail, MailCache* cache);
    ~TrashPurger();
    TrashPurger(const TrashPurger&) = delete;
    TrashPurger& operator=(const TrashPurger&) = delete;

    void start();
    void stop();
    // Kör nästa pass snarast, t.ex. när en användare har ändrat sin inställning.
    void kick();

private:
    void loop();
    void pass(Database& wdb);
    void purgeAccount(Database& wdb, const Account& account, int days);

    const Config& config_;
    Database& db_;
    MailService& mail_;
    MailCache* cache_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool started_ = false;
    bool stopping_ = false;
    bool kicked_ = false;
    std::thread thread_;
};

}  // namespace brevduva
