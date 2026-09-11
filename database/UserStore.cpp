#include "database/UserStore.hpp"

namespace forgefs::database {

UserStore::UserStore(std::filesystem::path db_path) : db_(std::move(db_path)) {}

bool UserStore::CreateUser(const std::string& username, const std::string& password_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        Statement ins(db_,
                       "INSERT INTO users(username, password_hash, created_at) "
                       "VALUES (?, ?, strftime('%s','now'));");
        ins.BindText(1, username);
        ins.BindText(2, password_hash);
        ins.Step();
        return true;
    } catch (const DatabaseError&) {
        return false;  // most likely a UNIQUE constraint violation on username
    }
}

std::optional<UserRecord> UserStore::FindUser(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT id, username, password_hash FROM users WHERE username = ?;");
    stmt.BindText(1, username);
    if (!stmt.Step()) return std::nullopt;

    UserRecord user;
    user.id = stmt.ColumnInt64(0);
    user.username = stmt.ColumnText(1);
    user.password_hash = stmt.ColumnText(2);
    return user;
}

}  // namespace forgefs::database
