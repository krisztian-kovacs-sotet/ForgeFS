#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forgefs::server {

struct NodeInfo {
    std::string id;
    std::string host;
    uint16_t port = 0;
};

// In-memory registry of storage nodes that have announced themselves to the
// coordinator via kRegisterNode. There's no persistence or expiry: a node
// that never re-registers just stays listed (health is determined by live
// probing at request time, not by registry state). Thread-safe so it's
// ready for Phase 7's concurrent server, even though only one thread calls
// it today.
class NodeRegistry {
public:
    void Register(const std::string& id, const std::string& host, uint16_t port);
    std::optional<NodeInfo> Get(const std::string& id) const;
    std::vector<NodeInfo> AllNodes() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, NodeInfo> nodes_;
};

}  // namespace forgefs::server
