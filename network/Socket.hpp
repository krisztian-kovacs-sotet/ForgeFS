#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "network/Tls.hpp"

// Forward-declared (OpenSSL's per-connection TLS state) so this header
// doesn't pull <openssl/ssl.h> into every TU that includes it; only
// Socket.cpp needs the real definition.
struct ssl_st;

namespace forgefs::net {

class SocketError : public std::runtime_error {
public:
    explicit SocketError(const std::string& what) : std::runtime_error(what) {}
};

// RAII wrapper around a POSIX TCP socket file descriptor, optionally
// upgraded to TLS after connecting. Move-only.
class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(int fd) noexcept : fd_(fd) {}
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // Resolves `host` (hostname or IP literal — works for Docker service
    // names too) and connects. Throws SocketError on failure. If `timeout`
    // is set, gives up on an unresponsive peer after that long instead of
    // blocking for the OS's default (often minutes) — used for liveness
    // probes where an unreachable host is an expected outcome.
    static TcpSocket Connect(const std::string& host, uint16_t port,
                              std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    // Creates a listening socket bound to 0.0.0.0:port.
    static TcpSocket Listen(uint16_t port, int backlog = 64);

    // Blocks until a client connects; returns the accepted connection.
    TcpSocket Accept() const;

    // Performs a TLS server-side handshake over this already-accepted
    // plain connection. After this succeeds, SendAll/RecvAll transparently
    // encrypt/decrypt through TLS. Throws SocketError on handshake failure.
    void UpgradeToTlsServer(TlsContext& ctx);

    // Performs a TLS client-side handshake (with SNI set to `hostname`)
    // over this already-connected plain connection.
    void UpgradeToTlsClient(TlsContext& ctx, const std::string& hostname);

    bool IsTls() const noexcept { return tls_ != nullptr; }

    // Writes exactly `size` bytes, looping across partial writes/EINTR.
    void SendAll(const void* buffer, std::size_t size) const;

    // Reads exactly `size` bytes, looping across partial reads/EINTR.
    // Returns false if the peer closed the connection before any bytes of
    // this call were read (a clean EOF at a message boundary). Throws on a
    // partial read followed by disconnect, since that indicates a truncated
    // message rather than a clean close.
    bool RecvAll(void* buffer, std::size_t size) const;

    void Close() noexcept;
    bool IsValid() const noexcept { return fd_ >= 0; }
    int Fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    ssl_st* tls_ = nullptr;
};

}  // namespace forgefs::net
