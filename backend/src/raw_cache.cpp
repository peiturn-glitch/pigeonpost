#include "raw_cache.hpp"

#include "crypto.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace brevduva {

RawCache::RawCache(std::string dir, std::uint64_t maxBytes) : dir_(std::move(dir)), maxBytes_(maxBytes) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    for (auto it = fs::recursive_directory_iterator(dir_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) totalBytes_ += it->file_size(ec);
    }
}

std::string RawCache::pathFor(long long accountId, const std::string& folder, std::uint32_t uid) const {
    std::string name = crypto::toHex(crypto::sha256(folder + '\n' + std::to_string(uid)));
    return (fs::path(dir_) / std::to_string(accountId) / (name + ".eml")).string();
}

std::optional<std::string> RawCache::get(long long accountId, const std::string& folder, std::uint32_t uid) {
    if (maxBytes_ == 0) return std::nullopt;
    std::string path = pathFor(accountId, folder, uid);
    std::lock_guard lock(mutex_);
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // Senast använd, för utrensningen.
    return ss.str();
}

void RawCache::put(long long accountId, const std::string& folder, std::uint32_t uid, const std::string& raw) {
    if (maxBytes_ == 0 || raw.size() > maxBytes_ / 4) return;
    std::string path = pathFor(accountId, folder, uid);
    std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::uint64_t previous = fs::exists(path, ec) ? fs::file_size(path, ec) : 0;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    }
    totalBytes_ = totalBytes_ - std::min(totalBytes_, previous) + raw.size();
    if (totalBytes_ > maxBytes_) evict();
}

void RawCache::purgeAccount(long long accountId) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::path dir = fs::path(dir_) / std::to_string(accountId);
    std::uint64_t bytes = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) bytes += it->file_size(ec);
    }
    fs::remove_all(dir, ec);
    totalBytes_ -= std::min(totalBytes_, bytes);
}

// Anropas med låset taget. Tar bort de äldst använda filerna tills 80 % av gränsen återstår.
void RawCache::evict() {
    struct Entry {
        fs::path path;
        fs::file_time_type used;
        std::uint64_t size;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) entries.push_back({it->path(), it->last_write_time(ec), it->file_size(ec)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    std::uint64_t total = 0;
    for (const auto& e : entries) total += e.size;
    for (const auto& e : entries) {
        if (total <= maxBytes_ / 5 * 4) break;
        if (fs::remove(e.path, ec)) total -= e.size;
    }
    totalBytes_ = total;
}

}  // namespace brevduva
