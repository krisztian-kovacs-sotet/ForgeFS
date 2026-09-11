#include "server/SessionManager.hpp"

#include "crypto/Random.hpp"

namespace forgefs::server {

std::string SessionManager::CreateSession(const std::string& username) {
    const std::string token = crypto::GenerateRandomToken();
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[token] = username;
    return token;
}

std::optional<std::string> SessionManager::UsernameForToken(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(token);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}

}  // namespace forgefs::server
