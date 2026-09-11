#include "database/ChunkStore.hpp"

#include <sqlite3.h>

namespace forgefs::database {

ChunkStore::ChunkStore(std::filesystem::path db_path, uint64_t chunk_size)
    : db_(db_path), chunk_size_(chunk_size) {}

std::vector<std::string> ChunkStore::OrphanedOfLocked(const std::vector<std::string>& chunk_ids) {
    std::vector<std::string> orphaned;
    for (const auto& id : chunk_ids) {
        Statement count(db_, "SELECT COUNT(*) FROM chunks WHERE chunk_id = ?;");
        count.BindText(1, id);
        count.Step();
        if (count.ColumnInt64(0) == 0) orphaned.push_back(id);
    }
    return orphaned;
}

std::optional<FileRecord> ChunkStore::FindLocked(const std::string& name) {
    FileRecord record;
    {
        Statement stmt(db_, "SELECT id, name, size, sha256 FROM files WHERE name = ?;");
        stmt.BindText(1, name);
        if (!stmt.Step()) return std::nullopt;
        record.id = stmt.ColumnInt64(0);
        record.name = stmt.ColumnText(1);
        record.size = static_cast<uint64_t>(stmt.ColumnInt64(2));
        record.sha256 = stmt.ColumnText(3);
    }

    Statement chunks_stmt(
        db_, "SELECT chunk_index, chunk_id, size, sha256 FROM chunks WHERE file_id = ? "
             "ORDER BY chunk_index ASC;");
    chunks_stmt.BindInt64(1, record.id);
    while (chunks_stmt.Step()) {
        ChunkRecord c;
        c.index = static_cast<uint32_t>(chunks_stmt.ColumnInt64(0));
        c.chunk_id = chunks_stmt.ColumnText(1);
        c.size = static_cast<uint64_t>(chunks_stmt.ColumnInt64(2));
        c.sha256 = chunks_stmt.ColumnText(3);
        record.chunks.push_back(std::move(c));
    }
    return record;
}

std::vector<std::string> ChunkStore::CommitFile(const std::string& name, uint64_t total_size,
                                                 const std::string& sha256,
                                                 const std::vector<ChunkRecord>& chunks) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> old_chunk_ids;
    if (const auto existing = FindLocked(name)) {
        for (const auto& c : existing->chunks) old_chunk_ids.push_back(c.chunk_id);
    }

    db_.Execute("BEGIN IMMEDIATE;");
    try {
        {
            Statement del(db_, "DELETE FROM files WHERE name = ?;");
            del.BindText(1, name);
            del.Step();
        }
        {
            Statement ins(db_,
                           "INSERT INTO files(name, size, sha256, created_at) "
                           "VALUES (?, ?, ?, strftime('%s','now'));");
            ins.BindText(1, name);
            ins.BindInt64(2, static_cast<int64_t>(total_size));
            ins.BindText(3, sha256);
            ins.Step();
        }
        const int64_t file_id = sqlite3_last_insert_rowid(db_.Handle());

        Statement ins_chunk(db_,
                             "INSERT INTO chunks(file_id, chunk_index, chunk_id, size, sha256) "
                             "VALUES (?, ?, ?, ?, ?);");
        for (const auto& c : chunks) {
            ins_chunk.Reset();
            ins_chunk.BindInt64(1, file_id);
            ins_chunk.BindInt64(2, c.index);
            ins_chunk.BindText(3, c.chunk_id);
            ins_chunk.BindInt64(4, static_cast<int64_t>(c.size));
            ins_chunk.BindText(5, c.sha256);
            ins_chunk.Step();
        }
        db_.Execute("COMMIT;");
    } catch (...) {
        db_.Execute("ROLLBACK;");
        throw;
    }

    return OrphanedOfLocked(old_chunk_ids);
}

std::vector<std::string> ChunkStore::DeleteFile(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto record = FindLocked(name);
    if (!record) return {};

    std::vector<std::string> chunk_ids;
    chunk_ids.reserve(record->chunks.size());
    for (const auto& c : record->chunks) chunk_ids.push_back(c.chunk_id);

    Statement del(db_, "DELETE FROM files WHERE name = ?;");
    del.BindText(1, name);
    del.Step();

    return OrphanedOfLocked(chunk_ids);
}

std::vector<std::string> ChunkStore::ReleaseChunks(const std::vector<std::string>& chunk_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    return OrphanedOfLocked(chunk_ids);
}

void ChunkStore::RecordChunkLocation(const std::string& chunk_id, const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement ins(db_, "INSERT OR IGNORE INTO chunk_locations(chunk_id, node_id) VALUES (?, ?);");
    ins.BindText(1, chunk_id);
    ins.BindText(2, node_id);
    ins.Step();
}

std::vector<std::string> ChunkStore::LocationsFor(const std::string& chunk_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> nodes;
    Statement stmt(db_, "SELECT node_id FROM chunk_locations WHERE chunk_id = ?;");
    stmt.BindText(1, chunk_id);
    while (stmt.Step()) nodes.push_back(stmt.ColumnText(0));
    return nodes;
}

void ChunkStore::ForgetChunkLocations(const std::string& chunk_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement del(db_, "DELETE FROM chunk_locations WHERE chunk_id = ?;");
    del.BindText(1, chunk_id);
    del.Step();
}

bool ChunkStore::Exists(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT 1 FROM files WHERE name = ?;");
    stmt.BindText(1, name);
    return stmt.Step();
}

std::optional<FileRecord> ChunkStore::Find(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FindLocked(name);
}

std::vector<FileRecord> ChunkStore::ListFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FileRecord> files;
    Statement stmt(db_, "SELECT id, name, size, sha256 FROM files ORDER BY name ASC;");
    while (stmt.Step()) {
        FileRecord r;
        r.id = stmt.ColumnInt64(0);
        r.name = stmt.ColumnText(1);
        r.size = static_cast<uint64_t>(stmt.ColumnInt64(2));
        r.sha256 = stmt.ColumnText(3);
        files.push_back(std::move(r));
    }
    return files;
}

std::vector<std::string> ChunkStore::AllChunkIds() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    Statement stmt(db_, "SELECT DISTINCT chunk_id FROM chunks;");
    while (stmt.Step()) ids.push_back(stmt.ColumnText(0));
    return ids;
}

}  // namespace forgefs::database
