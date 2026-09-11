#include "common/Logging.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace forgefs::common {

namespace {

std::mutex g_log_mutex;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo: return "INFO";
        case LogLevel::kWarn: return "WARN";
        case LogLevel::kError: return "ERROR";
    }
    return "?";
}

std::string Timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

}  // namespace

void Log(LogLevel level, const std::string& message) {
    std::ostringstream tid;
    tid << std::this_thread::get_id();

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stderr, "[%s] [%s] [tid:%s] %s\n", Timestamp().c_str(), LevelName(level),
                 tid.str().c_str(), message.c_str());
}

}  // namespace forgefs::common
