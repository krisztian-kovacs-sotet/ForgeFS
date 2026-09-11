#pragma once

#include <string>

namespace forgefs::common {

// Returns true if `name` is safe to use as a single path component under a
// server-controlled data directory: non-empty, contains no path separators
// or ".." traversal, and isn't "." or "..". Rejects anything that could
// escape the intended directory or address an unintended file.
bool IsSafeFileName(const std::string& name);

}  // namespace forgefs::common
