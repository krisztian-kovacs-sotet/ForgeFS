#include "server/ReplicationMonitor.hpp"

#include "common/Logging.hpp"
#include "database/ChunkStore.hpp"
#include "server/ChunkDistributor.hpp"

namespace forgefs::server {

ReplicationMonitor::ReplicationMonitor(std::filesystem::path db_path, NodeRegistry& registry,
                                        std::chrono::seconds interval)
    : db_path_(std::move(db_path)),
      registry_(registry),
      interval_(interval),
      thread_(&ReplicationMonitor::Run, this) {}

ReplicationMonitor::~ReplicationMonitor() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void ReplicationMonitor::Run() {
    // Its own SQLite connection, kept off the coordinator's request-serving
    // one: WAL mode (set in Database's constructor) lets the two coexist
    // without blocking each other much even though this runs on its own
    // thread while the accept loop runs on the main one.
    database::ChunkStore store(db_path_);
    ChunkDistributor distributor(store, registry_);

    while (!stop_) {
        std::this_thread::sleep_for(interval_);
        if (stop_) break;

        try {
            for (const auto& chunk_id : store.AllChunkIds()) {
                if (stop_) break;
                distributor.ReconcileChunk(chunk_id);
            }
        } catch (const std::exception& e) {
            common::LogWarn(std::string("replication monitor: ") + e.what());
        }
    }
}

}  // namespace forgefs::server
