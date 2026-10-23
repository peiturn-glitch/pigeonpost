#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace brevduva {

// Hela meddelanden (med bilagor) som filer på disk, så att bilagor och inbäddade bilder i ett nyligen öppnat
// meddelande inte behöver hämtas från mejlservern igen. De äldsta filerna tas bort när gränsen är nådd.
class RawCache {
public:
    RawCache(std::string dir, std::uint64_t maxBytes);

    std::optional<std::string> get(long long accountId, const std::string& folder, std::uint32_t uid);
    void put(long long accountId, const std::string& folder, std::uint32_t uid, const std::string& raw);
    // Tar bort kontots filer, t.ex. när mejlservern har numrerat om en mapp och UID:n inte längre stämmer.
    void purgeAccount(long long accountId);

private:
    std::string pathFor(long long accountId, const std::string& folder, std::uint32_t uid) const;
    void evict();

    std::string dir_;
    std::uint64_t maxBytes_;
    std::mutex mutex_;
    std::uint64_t totalBytes_ = 0;
};

}  // namespace brevduva
