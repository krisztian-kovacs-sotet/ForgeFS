#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace forgefs::client {

// Where `forgefs login` caches its session token so later CLI invocations
// (each a fresh process) don't need to re-authenticate: $HOME/.forgefs/token.
std::filesystem::path TokenFilePath();

// Writes `token` to TokenFilePath() with owner-only permissions (0600).
void SaveToken(const std::string& token);

std::optional<std::string> LoadToken();

void ClearToken();

}  // namespace forgefs::client
