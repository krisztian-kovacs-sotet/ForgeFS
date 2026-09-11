#include "network/Socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace forgefs::net {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
    throw SocketError(what + ": " + std::strerror(errno));
}

[[noreturn]] void ThrowTlsError(const std::string& what) {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    throw SocketError(what + ": " + buf);
}

}  // namespace

TcpSocket::~TcpSocket() { Close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_), tls_(other.tls_) {
    other.fd_ = -1;
    other.tls_ = nullptr;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        tls_ = other.tls_;
        other.fd_ = -1;
        other.tls_ = nullptr;
    }
    return *this;
}

void TcpSocket::Close() noexcept {
    if (tls_ != nullptr) {
        SSL_shutdown(tls_);  // best-effort close_notify; errors ignored during teardown
        SSL_free(tls_);
        tls_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

TcpSocket TcpSocket::Connect(const std::string& host, uint16_t port,
                              std::optional<std::chrono::milliseconds> timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        throw SocketError("failed to resolve host '" + host + "': " + gai_strerror(rc));
    }

    int fd = -1;
    for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
        fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) continue;

        int original_flags = 0;
        if (timeout.has_value()) {
            original_flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK);
        }

        const int connect_rc = ::connect(fd, candidate->ai_addr, candidate->ai_addrlen);
        bool connected = connect_rc == 0;

        if (!connected && timeout.has_value() && errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int poll_rc = ::poll(&pfd, 1, static_cast<int>(timeout->count()));
            if (poll_rc > 0 && (pfd.revents & POLLOUT) != 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 &&
                            so_error == 0;
            }
        }

        if (!connected) {
            ::close(fd);
            fd = -1;
            continue;
        }

        if (timeout.has_value()) {
            ::fcntl(fd, F_SETFL, original_flags);  // restore blocking mode
        }
        break;
    }
    ::freeaddrinfo(result);

    if (fd < 0) {
        throw SocketError("failed to connect to " + host + ":" + port_str);
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return TcpSocket(fd);
}

TcpSocket TcpSocket::Listen(uint16_t port, int backlog) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) ThrowErrno("socket() failed");

    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        ::close(fd);
        ThrowErrno("setsockopt(SO_REUSEADDR) failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        ThrowErrno("bind() to port " + std::to_string(port) + " failed");
    }

    if (::listen(fd, backlog) < 0) {
        ::close(fd);
        ThrowErrno("listen() failed");
    }

    return TcpSocket(fd);
}

TcpSocket TcpSocket::Accept() const {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    const int client_fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0) ThrowErrno("accept() failed");

    int one = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return TcpSocket(client_fd);
}

void TcpSocket::UpgradeToTlsServer(TlsContext& ctx) {
    SSL* ssl = SSL_new(ctx.Handle());
    if (ssl == nullptr) throw SocketError("SSL_new failed");

    SSL_set_fd(ssl, fd_);
    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        ThrowTlsError("TLS handshake (server) failed");
    }
    tls_ = ssl;
}

void TcpSocket::UpgradeToTlsClient(TlsContext& ctx, const std::string& hostname) {
    SSL* ssl = SSL_new(ctx.Handle());
    if (ssl == nullptr) throw SocketError("SSL_new failed");

    SSL_set_fd(ssl, fd_);
    SSL_set_tlsext_host_name(ssl, hostname.c_str());  // SNI
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        ThrowTlsError("TLS handshake (client) failed");
    }
    tls_ = ssl;
}

void TcpSocket::SendAll(const void* buffer, std::size_t size) const {
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    std::size_t sent = 0;

    if (tls_ != nullptr) {
        while (sent < size) {
            const int n = SSL_write(tls_, bytes + sent, static_cast<int>(size - sent));
            if (n <= 0) ThrowTlsError("SSL_write failed");
            sent += static_cast<std::size_t>(n);
        }
        return;
    }

    while (sent < size) {
        const ssize_t n = ::send(fd_, bytes + sent, size - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ThrowErrno("send() failed");
        }
        sent += static_cast<std::size_t>(n);
    }
}

bool TcpSocket::RecvAll(void* buffer, std::size_t size) const {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::size_t received = 0;

    if (tls_ != nullptr) {
        while (received < size) {
            const int n = SSL_read(tls_, bytes + received, static_cast<int>(size - received));
            if (n > 0) {
                received += static_cast<std::size_t>(n);
                continue;
            }
            const int err = SSL_get_error(tls_, n);
            if (err == SSL_ERROR_ZERO_RETURN) {
                if (received == 0) return false;  // clean EOF at a message boundary
                throw SocketError("TLS connection closed mid-message");
            }
            ThrowTlsError("SSL_read failed");
        }
        return true;
    }

    while (received < size) {
        const ssize_t n = ::recv(fd_, bytes + received, size - received, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ThrowErrno("recv() failed");
        }
        if (n == 0) {
            if (received == 0) return false;  // clean EOF at a message boundary
            throw SocketError("connection closed mid-message (expected " +
                               std::to_string(size) + " bytes, got " +
                               std::to_string(received) + ")");
        }
        received += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace forgefs::net
