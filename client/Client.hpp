#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "network/Socket.hpp"
#include "network/Tls.hpp"

namespace forgefs::client {

struct FileInfo {
    std::string name;
    uint64_t size;
};

struct VerifyResult {
    bool matches;
    std::string computed_sha256;
    std::string expected_sha256;
};

struct NodeInfo {
    std::string id;
    std::string host;
    uint32_t port;
    bool healthy;
};

struct StatusInfo {
    uint32_t node_count;
    uint32_t healthy_node_count;
    uint32_t file_count;
    uint64_t total_bytes;
};

class ClientError : public std::runtime_error {
public:
    explicit ClientError(const std::string& what) : std::runtime_error(what) {}
};

// Called periodically during Upload()/Download() with bytes transferred so
// far and the total, so the CLI can render progress.
using ProgressCallback = std::function<void(uint64_t transferred, uint64_t total)>;

// A short-lived connection to a ForgeFS server: one TCP connection per
// Client instance, used for a single command. The CLI creates a fresh
// Client for each invocation. Every request except Login() carries a
// session token automatically once one has been set (via Login() or
// SetToken()) — the server rejects anything else with "authentication
// required".
class Client {
public:
    Client(std::string host, uint16_t port);

    void Connect();
    void ConnectTls(net::TlsContext& tls_context);

    void SetToken(std::string token) { token_ = std::move(token); }

    // Exchanges credentials for a session token, remembers it on this
    // Client instance, and returns it so the caller can persist it for
    // future CLI invocations (see client/TokenStore.hpp).
    std::string Login(const std::string& username, const std::string& password);

    std::vector<FileInfo> List();

    // Streams the local file to the server in fixed-size chunks, hashing it
    // with SHA-256 as it goes. Returns the hex digest once the server has
    // confirmed it received the same bytes. Throws ClientError on mismatch.
    std::string Upload(const std::filesystem::path& local_path, const std::string& remote_name,
                        const ProgressCallback& on_progress = nullptr);

    // Streams the remote file to local_path, verifying the transfer against
    // the server's trailing SHA-256 digest. Returns the verified hex digest.
    // Throws ClientError if the digests don't match.
    std::string Download(const std::string& remote_name, const std::filesystem::path& local_path,
                          const ProgressCallback& on_progress = nullptr);

    void Delete(const std::string& remote_name);

    // Asks the server to recompute the SHA-256 of its stored copy of
    // remote_name from the chunks on disk and compare it against the hash
    // recorded at upload time, without transferring the file.
    VerifyResult Verify(const std::string& remote_name);

    // Cluster introspection: registered storage nodes (with a live health
    // probe per node) and a summary of files/bytes/node counts.
    std::vector<NodeInfo> Nodes();
    StatusInfo Status();

private:
    std::string host_;
    uint16_t port_;
    std::string token_;
    net::TcpSocket socket_;
};

}  // namespace forgefs::client
