#include "network/Tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace forgefs::net {

namespace {

[[noreturn]] void ThrowTlsError(const std::string& what) {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    throw TlsError(what + ": " + buf);
}

// Ensures OpenSSL's error strings/algorithms are initialized exactly once.
// A no-op on OpenSSL 1.1+/3.x (which self-initializes) but kept so this
// still works against older 1.0.x builds.
void EnsureOpenSslInitialized() {
    static const bool initialized = [] {
        SSL_library_init();
        SSL_load_error_strings();
        return true;
    }();
    (void)initialized;
}

}  // namespace

TlsContext::~TlsContext() {
    if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

TlsContext::TlsContext(TlsContext&& other) noexcept : ctx_(other.ctx_) { other.ctx_ = nullptr; }

TlsContext& TlsContext::operator=(TlsContext&& other) noexcept {
    if (this != &other) {
        if (ctx_ != nullptr) SSL_CTX_free(ctx_);
        ctx_ = other.ctx_;
        other.ctx_ = nullptr;
    }
    return *this;
}

TlsContext TlsContext::ServerContext(const std::string& cert_path, const std::string& key_path) {
    EnsureOpenSslInitialized();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) ThrowTlsError("failed to create server TLS context");

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        ThrowTlsError("failed to load certificate '" + cert_path + "'");
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        ThrowTlsError("failed to load private key '" + key_path + "'");
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        ThrowTlsError("certificate/private key mismatch");
    }

    return TlsContext(ctx);
}

TlsContext TlsContext::ClientContext(const std::string& ca_path) {
    EnsureOpenSslInitialized();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) ThrowTlsError("failed to create client TLS context");

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (!ca_path.empty()) {
        if (SSL_CTX_load_verify_locations(ctx, ca_path.c_str(), nullptr) != 1) {
            SSL_CTX_free(ctx);
            ThrowTlsError("failed to load CA file '" + ca_path + "'");
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    return TlsContext(ctx);
}

}  // namespace forgefs::net
