#include "crypto/Random.hpp"

#include <openssl/rand.h>

#include <stdexcept>
#include <vector>

#include "crypto/Hex.hpp"

namespace forgefs::crypto {

std::string GenerateRandomToken(size_t byte_length) {
    std::vector<unsigned char> buf(byte_length);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("failed to generate random token");
    }
    return ToHex(buf.data(), buf.size());
}

}  // namespace forgefs::crypto
