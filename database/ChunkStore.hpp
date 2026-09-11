#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"

namespace forgefs::database {

struct ChunkRecord {
    uint32_t index = 0;
    // The chunk's own SHA-256 hex digest — content-addressed, so identical
    // chunk content always gets the same id regardless of which file or
    // node it lives on.
    std::string chunk_id;
    uint64_t size = 0;
    std::string sha256;
};

struct FileRecord {
    int64_t id = 0;
    std::string name;
    uint64_t size = 0;
    std::string sha256;
    std::vector<ChunkRecord> chunks;  // ordered by index; empty from ListFiles()
};

constexpr uint64_t kDefaultChunkSize = 4ull * 1024 * 1024;  // 4 MiB, per the roadmap

// Pure metadata layer: which files exist, which chunks make each one up,
// and which storage node(s) currently hold each chunk. Knows nothing about
// the network — server::ChunkDistributor is what actually moves chunk
// bytes to/from nodes, then calls back into this class to record what
// happened. Chunks are reference-counted rather than owned by a single
// file, since identical content can be shared across files or across
// re-uploads of the same name.
//
// Internally thread-safe: every public method takes an internal mutex for
// its duration, since Phase 7 calls this concurrently from multiple
// connection-handling threads. The lock is scoped to the (fast, local)
// SQLite calls only — never held across network I/O, which happens in
// server::ChunkDistributor between calls into this class — so it doesn't
// serialize concurrent uploads/downloads against each other, only their
// brief metadata updates.
class ChunkStore {
public:
    ChunkStore(std::filesystem::path db_path, uint64_t chunk_size = kDefaultChunkSize);

    uint64_t chunk_size() const noexcept { return chunk_size_; }

    // Atomically replaces any existing file record named `name` with a new
    // one made of `chunks`. Returns chunk_ids that were only referenced by
    // the version being replaced, for the caller to release from storage
    // nodes.
    std::vector<std::string> CommitFile(const std::string& name, uint64_t total_size,
                                         const std::string& sha256,
                                         const std::vector<ChunkRecord>& chunks);

    // Removes `name`'s file/chunk rows. Returns chunk_ids that are now
    // unreferenced by any file, for the caller to release from storage
    // nodes.
    std::vector<std::string> DeleteFile(const std::string& name);

    // Filters `chunk_ids` down to the ones no `chunks` row references
    // anymore. Metadata bookkeeping only — doesn't touch node storage or
    // chunk_locations; used to undo a failed upload's partial writes.
    std::vector<std::string> ReleaseChunks(const std::vector<std::string>& chunk_ids);

    void RecordChunkLocation(const std::string& chunk_id, const std::string& node_id);
    std::vector<std::string> LocationsFor(const std::string& chunk_id);
    void ForgetChunkLocations(const std::string& chunk_id);

    bool Exists(const std::string& name);
    std::optional<FileRecord> Find(const std::string& name);
    std::vector<FileRecord> ListFiles();

    // Every chunk_id currently referenced by at least one file, i.e. the
    // set of chunks that should be kept replicated. Used by the background
    // replication monitor (Phase 6) to know what to check.
    std::vector<std::string> AllChunkIds();

private:
    // Do-the-work counterparts of the public methods above, assuming the
    // caller already holds mutex_. Exists so CommitFile()/DeleteFile() can
    // reuse Find()'s logic without recursively locking a non-recursive
    // mutex.
    std::optional<FileRecord> FindLocked(const std::string& name);
    std::vector<std::string> OrphanedOfLocked(const std::vector<std::string>& chunk_ids);

    std::mutex mutex_;
    Database db_;
    uint64_t chunk_size_;
};

}  // namespace forgefs::database
