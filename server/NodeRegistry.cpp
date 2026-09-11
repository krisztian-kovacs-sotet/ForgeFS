#include "server/NodeRegistry.hpp"

namespace forgefs::server {

void NodeRegistry::Register(const std::string& id, const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_[id] = NodeInfo{id, host, port};
}

std::optional<NodeInfo> NodeRegistry::Get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) return std::nullopt;
    return it->second;
}

std::vector<NodeInfo> NodeRegistry::AllNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeInfo> result;
    result.reserve(nodes_.size());
    for (const auto& [id, info] : nodes_) result.push_back(info);
    return result;
}

}  // namespace forgefs::server
