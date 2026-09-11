#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace forgefs::server {

class NodeClientError : public std::runtime_error {
public:
    explicit NodeClientError(const std::string& what) : std::runtime_error(what) {}
};

// Short-lived connection from the coordinator to a single storage node,
// speaking the node-protocol opcodes (HEALTH_CHECK/STORE_CHUNK/FETCH_CHUNK/
// DELETE_CHUNK). One instance per operation, mirroring client::Client.
class NodeClient {
public:
    NodeClient(std::string host, uint16_t port);

    // Connects with a short timeout and pings the node. Returns false
    // instead of throwing on any failure — used for liveness probing,
    // where an unreachable node is an expected, not exceptional, outcome.
    bool HealthCheck();

    void StoreChunk(const std::string& chunk_id, const std::vector<uint8_t>& data);
    std::vector<uint8_t> FetchChunk(const std::string& chunk_id);
    void DeleteChunk(const std::string& chunk_id);

private:
    std::string host_;
    uint16_t port_;
};

}  // namespace forgefs::server
