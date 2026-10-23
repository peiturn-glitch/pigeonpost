#include "zip_writer.hpp"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace brevduva {

namespace {

constexpr std::uint16_t kUtf8Names = 0x0800;
constexpr std::uint16_t kStored = 0;
constexpr std::uint16_t kDeflated = 8;
constexpr std::uint32_t kMax32 = 0xffffffffu;
constexpr std::uint16_t kMax16 = 0xffff;

void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

void put64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

// Rå deflate (utan zlib-huvud), som zip-formatet kräver.
std::string deflateRaw(const std::string& data) {
    z_stream zs{};
    if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("Could not start compression");
    }
    std::string out(deflateBound(&zs, static_cast<uLong>(data.size())), '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) throw std::runtime_error("Compression failed");
    return out;
}

}  // namespace

ZipWriter::ZipWriter(const std::string& path) : path_(path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) throw std::runtime_error("Could not create " + path + ": " + std::strerror(errno));
}

ZipWriter::~ZipWriter() {
    if (file_) std::fclose(file_);
}

void ZipWriter::write(const std::string& bytes) {
    if (!file_) throw std::runtime_error("The zip file is already closed");
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
        throw std::runtime_error("Could not write the backup file (is the disk full?)");
    }
    offset_ += bytes.size();
}

void ZipWriter::add(const std::string& name, const std::string& data, std::time_t modified) {
    // Ett enskilt meddelande blir aldrig så stort; då räcker 32-bitars storlekar i det lokala huvudet.
    if (data.size() >= kMax32) throw std::runtime_error("File too large for the zip archive: " + name);
    if (name.size() > kMax16) throw std::runtime_error("File name too long for the zip archive");

    Entry e;
    e.name = name;
    e.size = data.size();
    e.offset = offset_;
    e.crc = static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data.data()),
                                             static_cast<uInt>(data.size())));
    std::tm tm{};
    localtime_r(&modified, &tm);
    if (tm.tm_year < 80) tm = std::tm{0, 0, 0, 1, 0, 80, 0, 0, 0, 0, nullptr};  // DOS-datum börjar 1980.
    e.dosTime = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    e.dosDate = static_cast<std::uint16_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);

    std::string compressed = deflateRaw(data);
    bool store = compressed.size() >= data.size();
    e.method = store ? kStored : kDeflated;
    e.compressedSize = store ? data.size() : compressed.size();

    std::string header;
    put32(header, 0x04034b50);
    put16(header, 20);
    put16(header, kUtf8Names);
    put16(header, e.method);
    put16(header, e.dosTime);
    put16(header, e.dosDate);
    put32(header, e.crc);
    put32(header, static_cast<std::uint32_t>(e.compressedSize));
    put32(header, static_cast<std::uint32_t>(e.size));
    put16(header, static_cast<std::uint16_t>(name.size()));
    put16(header, 0);
    header += name;
    write(header);
    write(store ? data : compressed);
    entries_.push_back(std::move(e));
}

void ZipWriter::finish() {
    std::uint64_t centralOffset = offset_;
    for (const auto& e : entries_) {
        bool zip64 = e.offset >= kMax32;
        std::string extra;
        if (zip64) {
            put16(extra, 0x0001);
            put16(extra, 8);
            put64(extra, e.offset);
        }
        std::string c;
        put32(c, 0x02014b50);
        // Skapad på Unix: annars antar en del zip-program (Info-ZIP) att filnamnen är i OEM-teckentabell.
        put16(c, static_cast<std::uint16_t>((3 << 8) | (zip64 ? 45 : 20)));
        put16(c, zip64 ? 45 : 20);
        put16(c, kUtf8Names);
        put16(c, e.method);
        put16(c, e.dosTime);
        put16(c, e.dosDate);
        put32(c, e.crc);
        put32(c, static_cast<std::uint32_t>(e.compressedSize));
        put32(c, static_cast<std::uint32_t>(e.size));
        put16(c, static_cast<std::uint16_t>(e.name.size()));
        put16(c, static_cast<std::uint16_t>(extra.size()));
        put16(c, 0);  // Kommentar
        put16(c, 0);  // Disk
        put16(c, 0);  // Interna attribut
        // Unix-rättigheter i de övre 16 bitarna: katalog 0755 (plus MS-DOS-katalogbiten) eller fil 0644.
        bool directory = !e.name.empty() && e.name.back() == '/';
        put32(c, directory ? (0040755u << 16) | 0x10 : (0100644u << 16));
        put32(c, zip64 ? kMax32 : static_cast<std::uint32_t>(e.offset));
        c += e.name;
        c += extra;
        write(c);
    }
    std::uint64_t centralSize = offset_ - centralOffset;
    std::uint64_t count = entries_.size();

    std::string end;
    bool zip64 = count >= kMax16 || centralOffset >= kMax32 || centralSize >= kMax32;
    if (zip64) {
        std::uint64_t zip64EndOffset = offset_;
        put32(end, 0x06064b50);
        put64(end, 44);  // Storleken på resten av posten.
        put16(end, 45);
        put16(end, 45);
        put32(end, 0);
        put32(end, 0);
        put64(end, count);
        put64(end, count);
        put64(end, centralSize);
        put64(end, centralOffset);
        put32(end, 0x07064b50);
        put32(end, 0);
        put64(end, zip64EndOffset);
        put32(end, 1);
    }
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    // Värden som inte ryms sätts till max; läsaren hämtar dem då från ZIP64-posten ovan.
    put16(end, static_cast<std::uint16_t>(std::min<std::uint64_t>(count, kMax16)));
    put16(end, static_cast<std::uint16_t>(std::min<std::uint64_t>(count, kMax16)));
    put32(end, static_cast<std::uint32_t>(std::min<std::uint64_t>(centralSize, kMax32)));
    put32(end, static_cast<std::uint32_t>(std::min<std::uint64_t>(centralOffset, kMax32)));
    put16(end, 0);
    write(end);

    if (std::fflush(file_) != 0 || std::fclose(file_) != 0) {
        file_ = nullptr;
        throw std::runtime_error("Could not write the backup file (is the disk full?)");
    }
    file_ = nullptr;
}

}  // namespace brevduva
