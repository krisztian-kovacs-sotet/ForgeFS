#pragma once

#include <cstddef>
#include <string>

namespace forgefs::crypto {

// Cryptographically random hex-encoded token (OpenSSL RAND_bytes), e.g. for
// session identifiers. Default 32 bytes = 256 bits of entropy.
std::string GenerateRandomToken(size_t byte_length = 32);

}  // namespace forgefs::crypto
