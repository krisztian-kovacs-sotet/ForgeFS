#include "crypto/Sha256.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace forgefs::crypto {

namespace {

std::string BytesToHex(const unsigned char* data, unsigned int len) {
    static const char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<size_t>(len) * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHexDigits[data[i] >> 4]);
        out.push_back(kHexDigits[data[i] & 0x0F]);
    }
    return out;
}

}  // namespace

Sha256Streamer::Sha256Streamer() : ctx_(EVP_MD_CTX_new()) {
    if (ctx_ == nullptr || EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("failed to initialize SHA-256 context");
    }
}

Sha256Streamer::~Sha256Streamer() {
    if (ctx_ != nullptr) EVP_MD_CTX_free(ctx_);
}

void Sha256Streamer::Update(const uint8_t* data, size_t len) {
    if (len == 0) return;
    if (EVP_DigestUpdate(ctx_, data, len) != 1) {
        throw std::runtime_error("SHA-256 update failed");
    }
}

std::string Sha256Streamer::HexDigest() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx_, digest, &digest_len) != 1) {
        throw std::runtime_error("SHA-256 finalize failed");
    }
    return BytesToHex(digest, digest_len);
}

std::string Sha256HexOfFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file for hashing: " + path.string());
    }

    Sha256Streamer hasher;
    std::vector<uint8_t> buffer(64 * 1024);
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) hasher.Update(buffer.data(), static_cast<size_t>(n));
    }
    return hasher.HexDigest();
}

}  // namespace forgefs::crypto
