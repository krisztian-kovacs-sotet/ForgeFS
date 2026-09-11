#include "server/UploadSession.hpp"

#include <algorithm>

#include "crypto/Sha256.hpp"

namespace forgefs::server {

UploadSession::UploadSession(database::ChunkStore& store, ChunkDistributor& distributor,
                              std::string file_name)
    : store_(store), distributor_(distributor), file_name_(std::move(file_name)) {
    buffer_.reserve(static_cast<size_t>(store_.chunk_size()));
}

void UploadSession::Write(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const size_t space = static_cast<size_t>(store_.chunk_size()) - buffer_.size();
        const size_t take = std::min(space, len - offset);
        buffer_.insert(buffer_.end(), data + offset, data + offset + take);
        offset += take;
        if (buffer_.size() == static_cast<size_t>(store_.chunk_size())) {
            FlushChunk();
        }
    }
}

void UploadSession::FlushChunk() {
    if (buffer_.empty()) return;

    crypto::Sha256Streamer hasher;
    hasher.Update(buffer_.data(), buffer_.size());
    const std::string hash = hasher.HexDigest();

    distributor_.StoreChunk(hash, buffer_);  // throws if no healthy node is available

    database::ChunkRecord record;
    record.index = next_index_++;
    record.chunk_id = hash;
    record.size = buffer_.size();
    record.sha256 = hash;
    chunks_.push_back(std::move(record));

    buffer_.clear();
}

void UploadSession::Finish(uint64_t total_size, const std::string& sha256) {
    FlushChunk();  // flush whatever partial data remains as the final chunk
    const auto orphaned = store_.CommitFile(file_name_, total_size, sha256, chunks_);
    distributor_.ReleaseOrphanedChunks(orphaned);
}

void UploadSession::Abort() {
    std::vector<std::string> chunk_ids;
    chunk_ids.reserve(chunks_.size());
    for (const auto& c : chunks_) chunk_ids.push_back(c.chunk_id);

    const auto orphaned = store_.ReleaseChunks(chunk_ids);
    distributor_.ReleaseOrphanedChunks(orphaned);

    chunks_.clear();
    buffer_.clear();
}

}  // namespace forgefs::server
