#pragma once

#include <string>

namespace forgefs::crypto {

// PBKDF2-HMAC-SHA256 with a random 16-byte salt and 210,000 iterations
// (OWASP's 2023 minimum recommendation for PBKDF2-SHA256), encoded as one
// self-describing string so the scheme can change later without
// invalidating already-stored hashes:
//   pbkdf2-sha256$<iterations>$<salt-hex>$<derived-key-hex>
// The plaintext password is never stored or logged.
std::string HashPassword(const std::string& password);

// Recomputes the hash from `password` using the parameters embedded in
// `stored` and compares in constant time. Returns false (never throws) on
// any malformed input, so a corrupted stored hash just fails auth instead
// of crashing the handler.
bool VerifyPassword(const std::string& password, const std::string& stored);

}  // namespace forgefs::crypto
