#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace brevduva {

// Skriver en zip-fil direkt till disk, fil för fil, så att stora arkiv inte behöver ligga i minnet.
// Filerna komprimeras med deflate och ZIP64 används automatiskt när arkivet växer över 4 GB
// eller 65 535 filer.
class ZipWriter {
public:
    explicit ZipWriter(const std::string& path);
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    // name använder "/" som mappavgränsare och är UTF-8.
    void add(const std::string& name, const std::string& data, std::time_t modified);
    // Skriver den centrala katalogen och stänger filen.
    void finish();

    std::uint64_t bytesWritten() const { return offset_; }

private:
    struct Entry {
        std::string name;
        std::uint32_t crc = 0;
        std::uint64_t compressedSize = 0;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        std::uint16_t method = 0;
        std::uint16_t dosTime = 0;
        std::uint16_t dosDate = 0;
    };

    void write(const std::string& bytes);

    std::string path_;
    std::FILE* file_ = nullptr;
    std::vector<Entry> entries_;
    std::uint64_t offset_ = 0;
};

}  // namespace brevduva
