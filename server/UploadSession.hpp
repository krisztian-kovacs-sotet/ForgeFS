#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "database/ChunkStore.hpp"
#include "server/ChunkDistributor.hpp"

namespace forgefs::server {

// Buffers incoming upload bytes — which arrive in small network-sized
// pieces — into ChunkStore::chunk_size()-sized pieces, handing each one to
// a ChunkDistributor to place on a storage node as it fills. Call Finish()
// once every byte of the upload has streamed through to commit the file's
// metadata, or Abort() to discard a failed upload and release whatever
// chunks it already placed.
class UploadSession {
public:
    UploadSession(database::ChunkStore& store, ChunkDistributor& distributor, std::string file_name);

    void Write(const uint8_t* data, size_t len);
    void Finish(uint64_t total_size, const std::string& sha256);
    void Abort();

private:
    void FlushChunk();

    database::ChunkStore& store_;
    ChunkDistributor& distributor_;
    std::string file_name_;
    std::vector<uint8_t> buffer_;
    std::vector<database::ChunkRecord> chunks_;
    uint32_t next_index_ = 0;
};

}  // namespace forgefs::server
