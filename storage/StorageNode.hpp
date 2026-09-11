#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "common/ThreadPool.hpp"
#include "network/Socket.hpp"

namespace forgefs::storage {

constexpr size_t kDefaultThreadCount = 8;

// Storage node executable's server loop: stores and serves individual
// chunks as flat, content-addressed files under `data_dir`, on behalf of
// whichever coordinator placed them here. Deliberately dumb — it has no
// concept of files, only chunk ids and bytes; the coordinator owns all
// file/chunk metadata. Accepted connections are handed to a thread pool so
// several StoreChunk/FetchChunk requests from the coordinator can be served
// at once; concurrent writes to the same new chunk id are safe without
// extra locking because content-addressing guarantees they'd be writing
// identical bytes.
class StorageNode {
public:
    explicit StorageNode(uint16_t port, std::filesystem::path data_dir,
                          size_t thread_count = kDefaultThreadCount);

    // Binds the listening socket and serves connections until the process
    // is killed. Blocks the calling thread.
    void Run();

private:
    void HandleClient(net::TcpSocket client);

    void HandleHealthCheck(const net::TcpSocket& client);
    void HandleStoreChunk(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleFetchChunk(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleDeleteChunk(const net::TcpSocket& client, const std::vector<uint8_t>& payload);

    std::filesystem::path ChunkPath(const std::string& chunk_id) const;

    uint16_t port_;
    std::filesystem::path data_dir_;
    common::ThreadPool thread_pool_;
};

}  // namespace forgefs::storage
