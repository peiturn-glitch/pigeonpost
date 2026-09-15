#pragma once

#include <stdexcept>
#include <string>

namespace brevduva {

// Ogiltig indata från klienten -> HTTP 400.
struct BadRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Ingen giltig session -> HTTP 401.
struct Unauthorized : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Inloggad men saknar behörighet -> HTTP 403.
struct Forbidden : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Resursen finns inte (eller tillhör en annan användare) -> HTTP 404.
struct NotFound : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// För många misslyckade inloggningar -> HTTP 429.
struct TooManyRequests : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Nätverksfel mot mailserver (anslutning bruten, timeout). Anslutningen kan återupprättas.
struct NetError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Servern svarade NO/BAD eller avvisade inloggning -> HTTP 502.
struct MailServerError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace brevduva
