#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/ThreadPool.hpp"
#include "database/ChunkStore.hpp"
#include "database/UserStore.hpp"
#include "network/Socket.hpp"
#include "network/Tls.hpp"
#include "server/ChunkDistributor.hpp"
#include "server/NodeRegistry.hpp"
#include "server/ReplicationMonitor.hpp"
#include "server/SessionManager.hpp"

namespace forgefs::server {

constexpr size_t kDefaultThreadCount = 8;

// Coordinator: accepts client/node connections and hands each one to a
// thread pool, so multiple uploads/downloads/node requests are served
// concurrently rather than queued behind each other. Tracks file/chunk
// metadata (SQLite, via database::ChunkStore, internally mutex-protected)
// and delegates chunk storage to registered storage-node processes (via
// ChunkDistributor) rather than storing chunk bytes itself. Chunks are
// replicated across multiple nodes and a background ReplicationMonitor
// keeps them that way.
//
// Every client request except LOGIN must carry a valid session token
// (SessionManager); user accounts and password hashes live in UserStore.
// If constructed with TLS cert/key paths, the listening socket only speaks
// TLS — plaintext connections fail the handshake and are dropped.
class Server {
public:
    Server(uint16_t port, std::filesystem::path data_dir,
           size_t thread_count = kDefaultThreadCount, std::string tls_cert_path = "",
           std::string tls_key_path = "");

    // Binds the listening socket and serves connections until the process
    // is killed. Blocks the calling thread.
    void Run();

    // Creates a user account directly (bypassing the network), for
    // bootstrapping the first account from the CLI/entrypoint. Returns
    // false if the username is already taken.
    bool CreateUser(const std::string& username, const std::string& password);

private:
    void HandleClient(net::TcpSocket client);

    // Reads a token from the front of `payload`, advancing `offset` past
    // it, and validates it against session_manager_. On failure, sends
    // kError itself and returns nullopt — callers should just `return`.
    std::optional<std::string> Authorize(const net::TcpSocket& client,
                                          const std::vector<uint8_t>& payload, size_t& offset);

    void HandleLogin(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleList(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleUpload(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleDownload(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleDelete(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleVerify(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleRegisterNode(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleNodes(const net::TcpSocket& client, const std::vector<uint8_t>& payload);
    void HandleStatus(const net::TcpSocket& client, const std::vector<uint8_t>& payload);

    uint16_t port_;
    std::filesystem::path data_dir_;
    database::ChunkStore chunk_store_;
    database::UserStore user_store_;
    SessionManager session_manager_;
    NodeRegistry node_registry_;
    ChunkDistributor distributor_;
    ReplicationMonitor replication_monitor_;
    common::ThreadPool thread_pool_;
    std::optional<net::TlsContext> tls_context_;
};

}  // namespace forgefs::server
