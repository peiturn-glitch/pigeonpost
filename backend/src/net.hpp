#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <string>

namespace brevduva {

// En TCP-anslutning som kan uppgraderas till TLS. Blockerande I/O med timeout.
class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void connect(const std::string& host, int port, int timeoutSeconds = 30);
    void startTls(const std::string& host, bool verify);

    // Läser en rad inklusive avslutande CRLF.
    std::string readLine();
    std::string readBytes(std::size_t n);
    void write(const std::string& data);
    void close();

    bool isOpen() const { return fd_ >= 0; }
    bool isTls() const { return ssl_ != nullptr; }

private:
    void fill();

    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buf_;
    std::size_t pos_ = 0;
};

}  // namespace brevduva
