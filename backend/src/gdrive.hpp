#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace brevduva {

// En liten klient för Google Drive API v3 med behörigheten drive.file.
class GoogleDrive {
public:
    // Ger en access-token. forceRefresh är true när Google har avvisat den förra.
    using TokenProvider = std::function<std::string(bool forceRefresh)>;

    struct File {
        std::string id;
        std::string name;
        std::string createdTime;
        std::uint64_t size = 0;
    };

    GoogleDrive(std::string apiUrl, TokenProvider token);

    // Returnerar id för en mapp med namnet i Drive-roten. knownId återanvänds om mappen fortfarande finns.
    std::string ensureFolder(const std::string& name, const std::string& knownId);

    // Laddar upp en fil i bitar (återupptagbar uppladdning), så att stora filer klarar avbrott.
    // progress anropas efter varje bit och kan returnera false för att avbryta.
    File upload(const std::string& path, const std::string& name, const std::string& parentId,
                const std::map<std::string, std::string>& appProperties,
                const std::function<bool(std::uint64_t sent, std::uint64_t total)>& progress);

    // Filer i mappen med en viss appProperty, nyaste först.
    std::vector<File> listByProperty(const std::string& parentId, const std::string& key, const std::string& value);

    void remove(const std::string& fileId);

private:
    struct Response {
        long status = 0;  // 0 = nätverksfel
        std::string body;
        std::string error;
        std::map<std::string, std::string> headers;  // Gemener
    };

    Response send(const std::string& method, const std::string& url, const std::string& body,
                  const std::vector<std::string>& headers, bool longTransfer = false);
    // Som send, med access-token, en ny token vid 401 och nya försök vid tillfälliga fel.
    Response call(const std::string& method, const std::string& url, const std::string& body = {},
                  const std::vector<std::string>& headers = {});
    [[noreturn]] void fail(const std::string& action, const Response& res);

    std::string apiUrl_;
    TokenProvider token_;
};

}  // namespace brevduva
