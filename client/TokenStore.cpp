#include "client/TokenStore.hpp"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace forgefs::client {

std::filesystem::path TokenFilePath() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir =
        home != nullptr ? std::filesystem::path(home) / ".forgefs" : std::filesystem::path(".forgefs");
    std::filesystem::create_directories(dir);
    return dir / "token";
}

void SaveToken(const std::string& token) {
    const auto path = TokenFilePath();
    {
        std::ofstream out(path, std::ios::trunc);
        out << token;
    }
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);  // 0600: owner read/write only
}

std::optional<std::string> LoadToken() {
    std::ifstream in(TokenFilePath());
    if (!in) return std::nullopt;
    std::string token;
    std::getline(in, token);
    if (token.empty()) return std::nullopt;
    return token;
}

void ClearToken() {
    std::error_code ec;
    std::filesystem::remove(TokenFilePath(), ec);
}

}  // namespace forgefs::client
