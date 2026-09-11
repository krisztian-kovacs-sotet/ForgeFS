#pragma once

#include <stdexcept>
#include <string>

// Forward-declared so this header doesn't pull <openssl/ssl.h> into every
// TU that includes it; only Tls.cpp and Socket.cpp need the real
// definition.
struct ssl_ctx_st;

namespace forgefs::net {

class TlsError : public std::runtime_error {
public:
    explicit TlsError(const std::string& what) : std::runtime_error(what) {}
};

// Thin RAII wrapper around an OpenSSL SSL_CTX, configured once per process
// (loading a certificate/key for a server, or CA trust for a client) and
// reused for every connection made through it.
class TlsContext {
public:
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&& other) noexcept;
    TlsContext& operator=(TlsContext&& other) noexcept;

    // Loads a PEM certificate + private key for a TLS server.
    static TlsContext ServerContext(const std::string& cert_path, const std::string& key_path);

    // For a TLS client. If `ca_path` is non-empty, the peer certificate is
    // verified against it; otherwise verification is disabled entirely —
    // appropriate only for a self-signed dev/demo certificate whose CA
    // hasn't been distributed to clients out of band (see docs/SECURITY.md).
    static TlsContext ClientContext(const std::string& ca_path = "");

    ssl_ctx_st* Handle() const noexcept { return ctx_; }

private:
    explicit TlsContext(ssl_ctx_st* ctx) : ctx_(ctx) {}

    ssl_ctx_st* ctx_ = nullptr;
};

}  // namespace forgefs::net
