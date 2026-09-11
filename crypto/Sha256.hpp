#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <openssl/evp.h>

namespace forgefs::crypto {

// Incremental SHA-256 hasher over OpenSSL's EVP API. Feed data through any
// number of Update() calls, then call HexDigest() exactly once to finalize
// and get the lowercase hex digest. Used to hash data as it streams through
// the network layer, without ever holding a whole file in memory.
class Sha256Streamer {
public:
    Sha256Streamer();
    ~Sha256Streamer();

    Sha256Streamer(const Sha256Streamer&) = delete;
    Sha256Streamer& operator=(const Sha256Streamer&) = delete;

    void Update(const uint8_t* data, size_t len);
    std::string HexDigest();

private:
    EVP_MD_CTX* ctx_;
};

// Hashes a file on disk in fixed-size chunks (bounded memory use regardless
// of file size) and returns its SHA-256 as a lowercase hex string.
std::string Sha256HexOfFile(const std::filesystem::path& path);

}  // namespace forgefs::crypto
