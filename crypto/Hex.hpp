#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace forgefs::crypto {

std::string ToHex(const unsigned char* data, size_t len);

// Returns an empty vector if `hex` isn't valid even-length hex — callers
// that need to distinguish "empty input" from "malformed input" should
// check the source string's length themselves first.
std::vector<unsigned char> FromHex(const std::string& hex);

}  // namespace forgefs::crypto
