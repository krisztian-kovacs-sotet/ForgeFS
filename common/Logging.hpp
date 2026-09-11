#pragma once

#include <string>

namespace forgefs::common {

enum class LogLevel { kDebug, kInfo, kWarn, kError };

// Thread-safe: serializes writes to stderr so log lines from different
// connection-handling threads don't interleave (matters from Phase 7 on).
void Log(LogLevel level, const std::string& message);

inline void LogDebug(const std::string& message) { Log(LogLevel::kDebug, message); }
inline void LogInfo(const std::string& message) { Log(LogLevel::kInfo, message); }
inline void LogWarn(const std::string& message) { Log(LogLevel::kWarn, message); }
inline void LogError(const std::string& message) { Log(LogLevel::kError, message); }

}  // namespace forgefs::common
