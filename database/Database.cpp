#include "database/Database.hpp"

namespace forgefs::database {

Database::Database(const std::filesystem::path& db_path) {
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error";
        if (db_ != nullptr) sqlite3_close(db_);
        throw DatabaseError("failed to open database '" + db_path.string() + "': " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    Execute("PRAGMA foreign_keys = ON;");
    // WAL lets the coordinator's request-handling connection and the
    // background replication monitor's own connection (Phase 6) read/write
    // the same file concurrently without blocking each other as much as
    // the default rollback-journal mode would.
    Execute("PRAGMA journal_mode = WAL;");
    RunMigrations();
}

Database::~Database() {
    if (db_ != nullptr) sqlite3_close(db_);
}

void Database::Execute(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err != nullptr ? err : "unknown error";
        sqlite3_free(err);
        throw DatabaseError("sqlite exec failed: " + msg);
    }
}

void Database::RunMigrations() {
    Execute(R"sql(
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            size INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
    )sql");

    Execute(R"sql(
        CREATE TABLE IF NOT EXISTS chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            chunk_index INTEGER NOT NULL,
            chunk_id TEXT NOT NULL,
            size INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            UNIQUE(file_id, chunk_index)
        );
    )sql");

    Execute("CREATE INDEX IF NOT EXISTS idx_chunks_file_id ON chunks(file_id);");
    Execute("CREATE INDEX IF NOT EXISTS idx_chunks_chunk_id ON chunks(chunk_id);");

    // Which storage node(s) hold a copy of a given chunk. Kept separate
    // from `chunks` (rather than a node_id column there) so a chunk can
    // have more than one location once Phase 6 adds replication.
    Execute(R"sql(
        CREATE TABLE IF NOT EXISTS chunk_locations (
            chunk_id TEXT NOT NULL,
            node_id TEXT NOT NULL,
            PRIMARY KEY (chunk_id, node_id)
        );
    )sql");
    Execute("CREATE INDEX IF NOT EXISTS idx_chunk_locations_chunk_id ON chunk_locations(chunk_id);");

    // Phase 8: user accounts. password_hash is always crypto::HashPassword's
    // output (PBKDF2-HMAC-SHA256, self-describing) — never a plaintext
    // password.
    Execute(R"sql(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
    )sql");
}

Statement::Statement(Database& db, const std::string& sql) {
    if (sqlite3_prepare_v2(db.Handle(), sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw DatabaseError(std::string("failed to prepare statement: ") +
                             sqlite3_errmsg(db.Handle()));
    }
}

Statement::~Statement() {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
}

void Statement::BindText(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void Statement::BindInt64(int index, int64_t value) { sqlite3_bind_int64(stmt_, index, value); }

bool Statement::Step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DatabaseError(std::string("sqlite step failed: ") +
                         sqlite3_errmsg(sqlite3_db_handle(stmt_)));
}

std::string Statement::ColumnText(int index) const {
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    return text != nullptr ? reinterpret_cast<const char*>(text) : std::string();
}

int64_t Statement::ColumnInt64(int index) const { return sqlite3_column_int64(stmt_, index); }

void Statement::Reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

}  // namespace forgefs::database
