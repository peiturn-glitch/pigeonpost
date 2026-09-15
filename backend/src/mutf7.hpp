#pragma once

#include <string>

namespace brevduva {

// Konvertering mellan UTF-8 och IMAP:s modifierade UTF-7 (RFC 3501 5.1.3) för mappnamn.
std::string utf8ToMutf7(const std::string& utf8);
std::string mutf7ToUtf8(const std::string& mutf7);

}  // namespace brevduva
