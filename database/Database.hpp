#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

namespace forgefs::database {

class DatabaseError : public std::runtime_error {
public:
    explicit DatabaseError(const std::string& what) : std::runtime_error(what) {}
};

// RAII wrapper around a sqlite3 connection. Runs schema migrations on
// construction so callers always see an up-to-date `files`/`chunks` schema.
class Database {
public:
    explicit Database(const std::filesystem::path& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* Handle() const noexcept { return db_; }

    // Executes `sql` with no bound parameters and no result rows (DDL,
    // transaction control, etc.).
    void Execute(const std::string& sql);

private:
    void RunMigrations();
    sqlite3* db_ = nullptr;
};

// RAII wrapper around a prepared statement, for parameterized queries.
class Statement {
public:
    Statement(Database& db, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void BindText(int index, const std::string& value);
    void BindInt64(int index, int64_t value);

    // Advances one row. Returns true if a row is available (SQLITE_ROW),
    // false once the statement is exhausted (SQLITE_DONE).
    bool Step();

    std::string ColumnText(int index) const;
    int64_t ColumnInt64(int index) const;

    void Reset();

private:
    sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace forgefs::database
