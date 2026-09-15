#include "net.hpp"

#include "errors.hpp"

#include <netdb.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tbk {

namespace {

constexpr std::size_t kMaxLineLength = 16 * 1024 * 1024;

std::string opensslError() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "okänt TLS-fel";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}

}  // namespace

Connection::~Connection() { close(); }

void Connection::connect(const std::string& host, int port, int timeoutSeconds) {
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0) {
        throw NetError("Kunde inte slå upp " + host + ": " + gai_strerror(rc));
    }

    std::string lastError = "ingen adress";
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        timeval tv{};
        tv.tv_sec = timeoutSeconds;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd_ = fd;
            break;
        }
        lastError = std::strerror(errno);
        ::close(fd);
    }
    freeaddrinfo(result);

    if (fd_ < 0) {
        throw NetError("Kunde inte ansluta till " + host + ":" + portStr + ": " + lastError);
    }
}

void Connection::startTls(const std::string& host, bool verify) {
    if (fd_ < 0) throw NetError("Ingen anslutning");

    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) throw NetError("SSL_CTX_new: " + opensslError());
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    if (verify) {
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    } else {
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }

    ssl_ = SSL_new(ctx_);
    SSL_set_fd(ssl_, fd_);
    SSL_set_tlsext_host_name(ssl_, host.c_str());
    if (verify) SSL_set1_host(ssl_, host.c_str());

    if (SSL_connect(ssl_) != 1) {
        std::string err = opensslError();
        long verifyResult = SSL_get_verify_result(ssl_);
        if (verifyResult != X509_V_OK) {
            err = std::string("certifikatet kunde inte verifieras (") +
                  X509_verify_cert_error_string(verifyResult) + ")";
        }
        close();
        throw NetError("TLS-handskakning mot " + host + " misslyckades: " + err);
    }
}

void Connection::fill() {
    if (fd_ < 0) throw NetError("Anslutningen är stängd");

    // Komprimera bufferten när allt läst har konsumerats.
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 64 * 1024) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }

    char chunk[16 * 1024];
    int n;
    if (ssl_) {
        n = SSL_read(ssl_, chunk, sizeof(chunk));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, n);
            close();
            if (err == SSL_ERROR_ZERO_RETURN) throw NetError("Servern stängde anslutningen");
            throw NetError("Läsfel (TLS)");
        }
    } else {
        n = static_cast<int>(::recv(fd_, chunk, sizeof(chunk), 0));
        if (n <= 0) {
            std::string reason = n == 0 ? "Servern stängde anslutningen"
                                        : std::string("Läsfel: ") + std::strerror(errno);
            close();
            throw NetError(reason);
        }
    }
    buf_.append(chunk, static_cast<std::size_t>(n));
}

std::string Connection::readLine() {
    std::size_t searchFrom = pos_;
    while (true) {
        std::size_t nl = buf_.find('\n', searchFrom);
        if (nl != std::string::npos) {
            std::string line = buf_.substr(pos_, nl + 1 - pos_);
            pos_ = nl + 1;
            return line;
        }
        if (buf_.size() - pos_ > kMaxLineLength) {
            close();
            throw NetError("För lång rad från servern");
        }
        std::size_t consumedBefore = pos_;
        std::size_t sizeBefore = buf_.size();
        fill();
        // fill() kan ha flyttat bufferten; räkna om sökpositionen.
        searchFrom = pos_ + (sizeBefore - consumedBefore);
    }
}

std::string Connection::readBytes(std::size_t n) {
    while (buf_.size() - pos_ < n) fill();
    std::string out = buf_.substr(pos_, n);
    pos_ += n;
    return out;
}

void Connection::write(const std::string& data) {
    if (fd_ < 0) throw NetError("Anslutningen är stängd");
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n;
        if (ssl_) {
            n = SSL_write(ssl_, data.data() + sent, static_cast<int>(data.size() - sent));
        } else {
            n = static_cast<int>(
                ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL));
        }
        if (n <= 0) {
            close();
            throw NetError("Skrivfel mot servern");
        }
        sent += static_cast<std::size_t>(n);
    }
}

void Connection::close() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buf_.clear();
    pos_ = 0;
}

}  // namespace tbk
