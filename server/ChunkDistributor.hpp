#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "database/ChunkStore.hpp"
#include "server/NodeRegistry.hpp"

namespace forgefs::server {

// How many distinct nodes each chunk is placed on. StoreChunk degrades
// gracefully (as few as 1 copy) when fewer healthy nodes are available;
// ReconcileChunk tops a chunk back up to this many once more nodes are
// healthy again.
constexpr size_t kReplicationFactor = 3;

// Orchestrates chunk placement across storage nodes: picks nodes, moves
// bytes to/from them via NodeClient, and keeps database::ChunkStore's
// chunk_locations table in sync with what's actually out on the cluster.
// This is the layer that knows about the network; ChunkStore itself never
// does. Safe to call concurrently (Phase 7): chunk_store_ and registry_ are
// each internally synchronized, and the only state owned directly by this
// class is an atomic round-robin cursor.
class ChunkDistributor {
public:
    ChunkDistributor(database::ChunkStore& chunk_store, NodeRegistry& registry);

    // Picks up to kReplicationFactor distinct healthy registered nodes
    // (round-robin) and stores `data` on each, recording every successful
    // placement. Throws only if not even one node could take it.
    void StoreChunk(const std::string& chunk_id, const std::vector<uint8_t>& data);

    // Fetches chunk bytes from whichever known location responds first.
    // Throws if no location is on record or every known holder is
    // unreachable.
    std::vector<uint8_t> FetchChunk(const std::string& chunk_id);

    // Deletes chunk_ids that ChunkStore reported as orphaned (no `chunks`
    // row references them anymore): removes them from every node that
    // holds them, then forgets their locations. Best-effort — an
    // unreachable node just keeps the orphaned bytes for now.
    void ReleaseOrphanedChunks(const std::vector<std::string>& chunk_ids);

    // Checks chunk_id's current healthy replica count and, if it has
    // fallen below kReplicationFactor (e.g. a node holding it went down),
    // fetches a live copy from a healthy replica and places additional
    // copies on healthy nodes that don't already hold one. A no-op if the
    // chunk is already sufficiently replicated, and gives up quietly if no
    // replica is currently reachable to repair from.
    void ReconcileChunk(const std::string& chunk_id);

private:
    std::vector<NodeInfo> PickHealthyNodes(size_t count);

    database::ChunkStore& chunk_store_;
    NodeRegistry& registry_;
    std::atomic<size_t> round_robin_cursor_{0};
};

}  // namespace forgefs::server
