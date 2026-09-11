#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "database/Database.hpp"

namespace forgefs::database {

struct UserRecord {
    int64_t id = 0;
    std::string username;
    std::string password_hash;  // crypto::HashPassword's output; never plaintext
};

// User accounts. Internally thread-safe, same pattern as ChunkStore — one
// mutex around each (fast, local) SQLite call.
class UserStore {
public:
    explicit UserStore(std::filesystem::path db_path);

    // Returns false instead of throwing if the username is already taken.
    bool CreateUser(const std::string& username, const std::string& password_hash);
    std::optional<UserRecord> FindUser(const std::string& username);

private:
    std::mutex mutex_;
    Database db_;
};

}  // namespace forgefs::database
