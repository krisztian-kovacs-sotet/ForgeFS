#include "common/PathSafety.hpp"

namespace forgefs::common {

bool IsSafeFileName(const std::string& name) {
    if (name.empty() || name.size() > 255) return false;
    if (name == "." || name == "..") return false;

    for (const char c : name) {
        if (c == '/' || c == '\\' || c == '\0') return false;
    }

    // Reject any embedded ".." to be defensive even though the checks above
    // already rule out '/'-separated traversal components.
    if (name.find("..") != std::string::npos) return false;

    return true;
}

}  // namespace forgefs::common
