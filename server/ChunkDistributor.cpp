#include "server/ChunkDistributor.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

#include "crypto/Sha256.hpp"
#include "server/NodeClient.hpp"

namespace forgefs::server {

ChunkDistributor::ChunkDistributor(database::ChunkStore& chunk_store, NodeRegistry& registry)
    : chunk_store_(chunk_store), registry_(registry) {}

std::vector<NodeInfo> ChunkDistributor::PickHealthyNodes(size_t count) {
    const auto nodes = registry_.AllNodes();
    if (nodes.empty()) return {};

    // A single atomic fetch_add gives each concurrent caller a distinct
    // starting point without a lock — round-robin fairness doesn't need to
    // be exact, just race-free.
    const size_t start = round_robin_cursor_.fetch_add(1, std::memory_order_relaxed) % nodes.size();

    std::vector<NodeInfo> healthy;
    for (size_t i = 0; i < nodes.size() && healthy.size() < count; ++i) {
        const auto& candidate = nodes[(start + i) % nodes.size()];
        NodeClient client(candidate.host, candidate.port);
        if (client.HealthCheck()) healthy.push_back(candidate);
    }
    return healthy;
}

void ChunkDistributor::StoreChunk(const std::string& chunk_id, const std::vector<uint8_t>& data) {
    const auto targets = PickHealthyNodes(kReplicationFactor);
    if (targets.empty()) {
        throw std::runtime_error("no healthy storage nodes available");
    }

    size_t successes = 0;
    for (const auto& node : targets) {
        try {
            NodeClient client(node.host, node.port);
            client.StoreChunk(chunk_id, data);
            chunk_store_.RecordChunkLocation(chunk_id, node.id);
            ++successes;
        } catch (const std::exception&) {
            // This node just won't hold a copy; the others still might.
        }
    }
    if (successes == 0) {
        throw std::runtime_error("failed to store chunk " + chunk_id + " on any node");
    }
}

std::vector<uint8_t> ChunkDistributor::FetchChunk(const std::string& chunk_id) {
    const auto locations = chunk_store_.LocationsFor(chunk_id);
    if (locations.empty()) {
        throw std::runtime_error("no known location for chunk " + chunk_id);
    }

    for (const auto& node_id : locations) {
        const auto node = registry_.Get(node_id);
        if (!node) continue;
        try {
            NodeClient client(node->host, node->port);
            auto data = client.FetchChunk(chunk_id);

            // Content-addressed: chunk_id IS this chunk's expected SHA-256,
            // so a corrupted copy on this node is detectable with no
            // separately-stored checksum — just try the next replica
            // instead of returning bad bytes.
            crypto::Sha256Streamer hasher;
            hasher.Update(data.data(), data.size());
            if (hasher.HexDigest() != chunk_id) continue;

            return data;
        } catch (const std::exception&) {
            continue;  // try the next replica
        }
    }
    throw std::runtime_error("chunk " + chunk_id + " is unreachable or corrupted on all known replicas");
}

void ChunkDistributor::ReleaseOrphanedChunks(const std::vector<std::string>& chunk_ids) {
    for (const auto& chunk_id : chunk_ids) {
        const auto locations = chunk_store_.LocationsFor(chunk_id);
        for (const auto& node_id : locations) {
            const auto node = registry_.Get(node_id);
            if (!node) continue;
            try {
                NodeClient client(node->host, node->port);
                client.DeleteChunk(chunk_id);
            } catch (const std::exception&) {
                // Best-effort: an unreachable node just keeps the bytes.
            }
        }
        chunk_store_.ForgetChunkLocations(chunk_id);
    }
}

void ChunkDistributor::ReconcileChunk(const std::string& chunk_id) {
    const auto locations = chunk_store_.LocationsFor(chunk_id);

    std::vector<std::string> healthy_holders;
    std::optional<std::vector<uint8_t>> chunk_data;
    for (const auto& node_id : locations) {
        const auto node = registry_.Get(node_id);
        if (!node) continue;
        NodeClient client(node->host, node->port);
        if (!client.HealthCheck()) continue;

        healthy_holders.push_back(node_id);
        if (!chunk_data.has_value()) {
            try {
                chunk_data = client.FetchChunk(chunk_id);
            } catch (const std::exception&) {
                // Reachable but this particular fetch failed; another
                // healthy holder (if any) may still supply a copy.
            }
        }
    }

    if (healthy_holders.size() >= kReplicationFactor) return;  // already sufficiently replicated
    if (!chunk_data.has_value()) return;  // nothing reachable to repair from right now

    const size_t needed = kReplicationFactor - healthy_holders.size();
    size_t placed = 0;
    for (const auto& candidate : registry_.AllNodes()) {
        if (placed >= needed) break;
        if (std::find(locations.begin(), locations.end(), candidate.id) != locations.end()) {
            continue;  // already a recorded holder (healthy or not)
        }
        NodeClient client(candidate.host, candidate.port);
        if (!client.HealthCheck()) continue;
        try {
            client.StoreChunk(chunk_id, *chunk_data);
            chunk_store_.RecordChunkLocation(chunk_id, candidate.id);
            ++placed;
        } catch (const std::exception&) {
            // Try the next candidate.
        }
    }
}

}  // namespace forgefs::server
