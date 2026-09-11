#include "crypto/PasswordHash.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <sstream>
#include <stdexcept>
#include <vector>

#include "crypto/Hex.hpp"

namespace forgefs::crypto {

namespace {

constexpr int kIterations = 210000;
constexpr int kSaltBytes = 16;
constexpr int kKeyBytes = 32;

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, delim)) parts.push_back(part);
    return parts;
}

}  // namespace

std::string HashPassword(const std::string& password) {
    unsigned char salt[kSaltBytes];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        throw std::runtime_error("failed to generate random salt");
    }

    unsigned char key[kKeyBytes];
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, sizeof(salt),
                           kIterations, EVP_sha256(), sizeof(key), key) != 1) {
        throw std::runtime_error("PBKDF2 derivation failed");
    }

    std::ostringstream oss;
    oss << "pbkdf2-sha256$" << kIterations << "$" << ToHex(salt, sizeof(salt)) << "$"
        << ToHex(key, sizeof(key));
    return oss.str();
}

bool VerifyPassword(const std::string& password, const std::string& stored) {
    const auto parts = Split(stored, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2-sha256") return false;

    int iterations = 0;
    try {
        iterations = std::stoi(parts[1]);
    } catch (const std::exception&) {
        return false;
    }
    if (iterations <= 0) return false;

    const auto salt = FromHex(parts[2]);
    const auto expected_key = FromHex(parts[3]);
    if (salt.empty() || expected_key.empty()) return false;

    std::vector<unsigned char> derived(expected_key.size());
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                           static_cast<int>(salt.size()), iterations, EVP_sha256(),
                           static_cast<int>(derived.size()), derived.data()) != 1) {
        return false;
    }

    // Constant-time comparison so a failed login doesn't leak timing
    // information about how many leading bytes matched.
    return CRYPTO_memcmp(derived.data(), expected_key.data(), derived.size()) == 0;
}

}  // namespace forgefs::crypto
