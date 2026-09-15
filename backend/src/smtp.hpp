#pragma once

#include "db.hpp"

#include <string>
#include <vector>

namespace tbk {

// Skickar ett färdigformaterat RFC 5322-meddelande via SMTP (libcurl).
void smtpSend(const Account& account, const std::string& from, const std::vector<std::string>& recipients,
              const std::string& message);

// Ansluter och autentiserar utan att skicka något.
void smtpTest(const Account& account);

}  // namespace tbk
