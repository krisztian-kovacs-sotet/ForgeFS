#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace forgefs::server {

// In-memory bearer-token sessions: LOGIN exchanges a validated
// username/password for a random token, and every other client request
// presents that token instead of resending credentials. Sessions don't
// survive a coordinator restart (a client just logs in again) — simple and
// sufficient for this project's scope; a production system would likely
// persist sessions or use short-lived signed tokens (JWTs) instead.
class SessionManager {
public:
    std::string CreateSession(const std::string& username);
    std::optional<std::string> UsernameForToken(const std::string& token) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> sessions_;  // token -> username
};

}  // namespace forgefs::server
