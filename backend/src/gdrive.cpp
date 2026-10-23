#include "gdrive.hpp"

#include "errors.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

namespace brevduva {

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kChunkSize = 8 * 1024 * 1024;  // Drive kräver multipler av 256 KiB.
constexpr int kAttempts = 6;
const char* kFolderMime = "application/vnd.google-apps.folder";

std::string urlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

// Strängliteral i Drives frågespråk.
std::string queryString(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    return out + "'";
}

std::size_t collectBody(char* data, std::size_t size, std::size_t n, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * n);
    return size * n;
}

std::size_t collectHeader(char* data, std::size_t size, std::size_t n, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(data, size * n);
    std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(colon + 1);
        std::size_t start = value.find_first_not_of(" \t");
        std::size_t end = value.find_last_not_of(" \t\r\n");
        (*headers)[key] = start == std::string::npos ? "" : value.substr(start, end - start + 1);
    }
    return size * n;
}

bool transient(long status) {
    return status == 0 || status == 429 || status >= 500;
}

void backoff(int attempt) {
    std::this_thread::sleep_for(std::chrono::seconds(std::min(60, 2 << attempt)));
}

GoogleDrive::File fileFromJson(const json& j) {
    GoogleDrive::File f;
    f.id = j.value("id", "");
    f.name = j.value("name", "");
    f.createdTime = j.value("createdTime", "");
    if (j.contains("size") && j["size"].is_string()) f.size = std::stoull(j["size"].get<std::string>());
    return f;
}

}  // namespace

GoogleDrive::GoogleDrive(std::string apiUrl, TokenProvider token) : apiUrl_(std::move(apiUrl)), token_(std::move(token)) {}

GoogleDrive::Response GoogleDrive::send(const std::string& method, const std::string& url, const std::string& body,
                                        const std::vector<std::string>& headers, bool longTransfer) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    Response res;
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    CURL* c = curl.get();

    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    list = curl_slist_append(list, "Expect:");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerList(list, &curl_slist_free_all);

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (method != "GET" && method != "DELETE") {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headerList.get());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collectBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, collectHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    if (longTransfer) {
        // En bit kan ta tid på en långsam uppkoppling; avbryt bara om överföringen står still.
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
    } else {
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    }
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        res.status = 0;
        res.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
        return res;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    return res;
}

GoogleDrive::Response GoogleDrive::call(const std::string& method, const std::string& url, const std::string& body,
                                        const std::vector<std::string>& headers) {
    bool refreshed = false;
    bool forceRefresh = false;
    for (int attempt = 0;; ++attempt) {
        std::vector<std::string> all = headers;
        all.push_back("Authorization: Bearer " + token_(forceRefresh));
        forceRefresh = false;
        Response res = send(method, url, body, all);
        if (res.status == 401 && !refreshed) {
            refreshed = forceRefresh = true;
            continue;
        }
        if (transient(res.status) && attempt + 1 < kAttempts) {
            backoff(attempt);
            continue;
        }
        return res;
    }
}

void GoogleDrive::fail(const std::string& action, const Response& res) {
    if (res.status == 0) throw NetError("Could not reach Google Drive: " + res.error);
    std::string message;
    std::string reason;
    json j = json::parse(res.body, nullptr, false);
    if (!j.is_discarded() && j.contains("error") && j["error"].is_object()) {
        message = j["error"].value("message", "");
        if (j["error"].contains("errors") && j["error"]["errors"].is_array() && !j["error"]["errors"].empty()) {
            reason = j["error"]["errors"][0].value("reason", "");
        }
    }
    if (reason == "storageQuotaExceeded") throw MailServerError("Google Drive is full. Free up space and try again.");
    if (reason == "accessNotConfigured" || message.find("has not been used in project") != std::string::npos) {
        throw MailServerError("The Google Drive API is not enabled in the Google Cloud project. Enable it and try again.");
    }
    if (res.status == 401 || res.status == 403) {
        throw MailServerError(action + ": Google Drive denied access (" + std::to_string(res.status) +
                              (message.empty() ? "" : ", " + message) + "). Connect Google Drive again.");
    }
    throw MailServerError(action + ": Google Drive responded " + std::to_string(res.status) +
                          (message.empty() ? "" : " (" + message + ")"));
}

std::string GoogleDrive::ensureFolder(const std::string& name, const std::string& knownId) {
    if (!knownId.empty()) {
        Response res = call("GET", apiUrl_ + "/drive/v3/files/" + urlEncode(knownId) + "?fields=id,trashed");
        if (res.status == 200) {
            json j = json::parse(res.body, nullptr, false);
            if (!j.is_discarded() && !j.value("trashed", false)) return knownId;
        } else if (res.status != 404) {
            fail("Could not find the backup folder", res);
        }
    }

    std::string q = "name = " + queryString(name) + " and mimeType = '" + kFolderMime + "' and trashed = false";
    Response found = call("GET", apiUrl_ + "/drive/v3/files?spaces=drive&fields=files(id)&q=" + urlEncode(q));
    if (found.status != 200) fail("Could not search for the backup folder", found);
    json list = json::parse(found.body, nullptr, false);
    if (!list.is_discarded() && list.contains("files") && list["files"].is_array() && !list["files"].empty()) {
        return list["files"][0].value("id", "");
    }

    json meta = {{"name", name}, {"mimeType", kFolderMime}};
    Response created = call("POST", apiUrl_ + "/drive/v3/files?fields=id", meta.dump(),
                            {"Content-Type: application/json; charset=UTF-8"});
    if (created.status != 200) fail("Could not create the backup folder", created);
    json j = json::parse(created.body, nullptr, false);
    std::string id = j.is_discarded() ? "" : j.value("id", "");
    if (id.empty()) throw MailServerError("Google Drive did not return an id for the backup folder");
    return id;
}

GoogleDrive::File GoogleDrive::upload(const std::string& path, const std::string& name, const std::string& parentId,
                                      const std::map<std::string, std::string>& appProperties,
                                      const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    struct Closer {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };
    std::unique_ptr<std::FILE, Closer> file(std::fopen(path.c_str(), "rb"));
    if (!file) throw std::runtime_error("Could not open " + path + ": " + std::strerror(errno));
    if (fseeko(file.get(), 0, SEEK_END) != 0) throw std::runtime_error("Could not read the backup file");
    auto total = static_cast<std::uint64_t>(ftello(file.get()));

    json meta = {{"name", name}, {"parents", {parentId}}, {"mimeType", "application/zip"}};
    meta["appProperties"] = json::object();
    for (const auto& [k, v] : appProperties) meta["appProperties"][k] = v;

    auto startSession = [&]() {
        Response res = call("POST", apiUrl_ + "/upload/drive/v3/files?uploadType=resumable&fields=id,name,createdTime,size",
                            meta.dump(),
                            {"Content-Type: application/json; charset=UTF-8", "X-Upload-Content-Type: application/zip",
                             "X-Upload-Content-Length: " + std::to_string(total)});
        if (res.status != 200 || res.headers["location"].empty()) fail("Could not start the upload", res);
        return res.headers["location"];
    };

    // Svar 308 betyder att mer data väntas; Range anger vad servern redan har tagit emot.
    auto confirmedOffset = [](const Response& res) -> std::uint64_t {
        auto it = res.headers.find("range");
        if (it == res.headers.end()) return 0;
        std::size_t dash = it->second.find('-');
        if (dash == std::string::npos) return 0;
        return std::stoull(it->second.substr(dash + 1)) + 1;
    };

    std::string session = startSession();
    std::uint64_t offset = 0;
    int failures = 0;
    bool restarted = false;
    std::string chunk;

    while (true) {
        std::uint64_t length = std::min(kChunkSize, total - offset);
        chunk.resize(length);
        if (fseeko(file.get(), static_cast<off_t>(offset), SEEK_SET) != 0 ||
            std::fread(chunk.data(), 1, length, file.get()) != length) {
            throw std::runtime_error("Could not read the backup file");
        }
        std::string range = total == 0 ? "bytes */0"
                                       : "bytes " + std::to_string(offset) + "-" + std::to_string(offset + length - 1) +
                                             "/" + std::to_string(total);
        Response res = send("PUT", session, chunk,
                            {"Authorization: Bearer " + token_(false), "Content-Type: application/zip",
                             "Content-Range: " + range},
                            true);

        if (res.status == 200 || res.status == 201) {
            if (progress) progress(total, total);
            json j = json::parse(res.body, nullptr, false);
            if (j.is_discarded()) throw MailServerError("Invalid response from Google Drive after the upload");
            return fileFromJson(j);
        }
        if (res.status == 308) {
            offset = confirmedOffset(res);
            failures = 0;
            if (progress && !progress(offset, total)) throw std::runtime_error("Upload cancelled");
            continue;
        }
        if ((res.status == 404 || res.status == 410) && !restarted) {
            // Uppladdningssessionen har gått ut; börja om från början en gång.
            restarted = true;
            session = startSession();
            offset = 0;
            continue;
        }
        if (!(transient(res.status) || res.status == 401) || ++failures >= kAttempts) {
            fail("Upload to Google Drive failed", res);
        }
        backoff(failures);
        // Fråga hur mycket servern har tagit emot innan nästa försök.
        Response status = send("PUT", session, "",
                               {"Authorization: Bearer " + token_(res.status == 401), "Content-Range: bytes */" +
                                                                                           std::to_string(total)});
        if (status.status == 200 || status.status == 201) {
            json j = json::parse(status.body, nullptr, false);
            if (!j.is_discarded()) return fileFromJson(j);
        }
        if (status.status == 308) offset = confirmedOffset(status);
    }
}

std::vector<GoogleDrive::File> GoogleDrive::listByProperty(const std::string& parentId, const std::string& key,
                                                           const std::string& value) {
    std::string q = queryString(parentId) + " in parents and trashed = false and appProperties has { key=" +
                    queryString(key) + " and value=" + queryString(value) + " }";
    std::vector<File> out;
    std::string pageToken;
    do {
        std::string url = apiUrl_ + "/drive/v3/files?spaces=drive&pageSize=100&orderBy=" + urlEncode("createdTime desc") +
                          "&fields=" + urlEncode("nextPageToken,files(id,name,createdTime,size)") + "&q=" + urlEncode(q);
        if (!pageToken.empty()) url += "&pageToken=" + urlEncode(pageToken);
        Response res = call("GET", url);
        if (res.status != 200) fail("Could not list previous backups", res);
        json j = json::parse(res.body, nullptr, false);
        if (j.is_discarded()) break;
        for (const auto& f : j.value("files", json::array())) out.push_back(fileFromJson(f));
        pageToken = j.value("nextPageToken", "");
    } while (!pageToken.empty());
    return out;
}

void GoogleDrive::remove(const std::string& fileId) {
    Response res = call("DELETE", apiUrl_ + "/drive/v3/files/" + urlEncode(fileId));
    if (res.status != 204 && res.status != 200 && res.status != 404) fail("Could not delete an old backup", res);
}

}  // namespace brevduva
