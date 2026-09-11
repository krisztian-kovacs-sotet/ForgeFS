#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "server/NodeRegistry.hpp"

namespace forgefs::server {

// Background maintenance loop, separate from client/node request handling:
// periodically checks every known chunk's replica health and re-replicates
// any that have fallen under ChunkDistributor::kReplicationFactor — e.g.
// because a storage node went down. This is one dedicated worker thread for
// anti-entropy, not the general connection-handling thread pool Phase 7
// adds — it exists purely to make "automatically create a replacement
// replica" actually automatic rather than only happening opportunistically
// on read (ChunkDistributor::FetchChunk already fails over across
// replicas; this is what proactively rebuilds them).
class ReplicationMonitor {
public:
    ReplicationMonitor(std::filesystem::path db_path, NodeRegistry& registry,
                        std::chrono::seconds interval = std::chrono::seconds(30));
    ~ReplicationMonitor();

    ReplicationMonitor(const ReplicationMonitor&) = delete;
    ReplicationMonitor& operator=(const ReplicationMonitor&) = delete;

private:
    void Run();

    std::filesystem::path db_path_;
    NodeRegistry& registry_;
    std::chrono::seconds interval_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace forgefs::server
