#pragma once

#include "ai_sort.hpp"
#include "auth.hpp"
#include "backup.hpp"
#include "db.hpp"
#include "mail_cache.hpp"
#include "mail_service.hpp"
#include "oauth.hpp"
#include "push.hpp"

namespace httplib {
class Server;
}

namespace brevduva {

// mailCache och push är nullptr när de inte är påslagna.
void registerApi(httplib::Server& server, Database& db, Auth& auth, MailService& mail, GoogleOAuth& oauth,
                 BackupService& backup, AiSorter& aiSorter, MailCache* mailCache, PushService* push);

}  // namespace brevduva
