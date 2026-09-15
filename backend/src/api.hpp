#pragma once

#include "auth.hpp"
#include "db.hpp"
#include "mail_service.hpp"
#include "oauth.hpp"

namespace httplib {
class Server;
}

namespace tbk {

void registerApi(httplib::Server& server, Database& db, Auth& auth, MailService& mail, GoogleOAuth& oauth);

}  // namespace tbk
