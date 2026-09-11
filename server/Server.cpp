#include "server/Server.hpp"

#include <algorithm>
#include <memory>

#include "common/Logging.hpp"
#include "common/PathSafety.hpp"
#include "crypto/PasswordHash.hpp"
#include "crypto/Sha256.hpp"
#include "protocol/Protocol.hpp"
#include "server/NodeClient.hpp"
#include "server/UploadSession.hpp"

namespace forgefs::server {

using forgefs::common::IsSafeFileName;
using forgefs::common::LogError;
using forgefs::common::LogInfo;
using forgefs::common::LogWarn;
using forgefs::protocol::AppendString;
using forgefs::protocol::AppendUint32;
using forgefs::protocol::AppendUint64;
using forgefs::protocol::AppendUint8;
using forgefs::protocol::ConnectionClosed;
using forgefs::protocol::kTransferChunkSize;
using forgefs::protocol::Message;
using forgefs::protocol::Opcode;
using forgefs::protocol::OpcodeName;
using forgefs::protocol::ProtocolError;
using forgefs::protocol::ReadString;
using forgefs::protocol::ReadUint32;
using forgefs::protocol::ReadUint64;
using forgefs::protocol::ReceiveMessage;
using forgefs::protocol::SendError;
using forgefs::protocol::SendMessage;

namespace {

// Ensures `path` exists before it's used to open the metadata DB, which
// happens during member initialization — before the constructor body would
// otherwise run.
std::filesystem::path EnsureDirectory(std::filesystem::path path) {
    std::filesystem::create_directories(path);
    return path;
}

std::optional<net::TlsContext> MaybeLoadServerTls(const std::string& cert_path,
                                                    const std::string& key_path) {
    if (cert_path.empty() && key_path.empty()) return std::nullopt;
    if (cert_path.empty() || key_path.empty()) {
        throw std::runtime_error("TLS requires both a certificate and a key path");
    }
    return net::TlsContext::ServerContext(cert_path, key_path);
}

}  // namespace

Server::Server(uint16_t port, std::filesystem::path data_dir, size_t thread_count,
               std::string tls_cert_path, std::string tls_key_path)
    : port_(port),
      data_dir_(EnsureDirectory(std::move(data_dir))),
      chunk_store_(data_dir_ / "metadata.db"),
      user_store_(data_dir_ / "metadata.db"),
      session_manager_(),
      node_registry_(),
      distributor_(chunk_store_, node_registry_),
      replication_monitor_(data_dir_ / "metadata.db", node_registry_),
      thread_pool_(thread_count),
      tls_context_(MaybeLoadServerTls(tls_cert_path, tls_key_path)) {}

bool Server::CreateUser(const std::string& username, const std::string& password) {
    return user_store_.CreateUser(username, crypto::HashPassword(password));
}

void Server::Run() {
    net::TcpSocket listener = net::TcpSocket::Listen(port_);
    LogInfo("ForgeFS coordinator listening on port " + std::to_string(port_) +
            ", data dir: " + data_dir_.string() + ", chunk size: " +
            std::to_string(chunk_store_.chunk_size()) + " bytes, replication factor: " +
            std::to_string(kReplicationFactor) + ", TLS: " +
            (tls_context_.has_value() ? "on" : "off"));

    // Each accepted connection is handed to the thread pool so multiple
    // clients/nodes are served concurrently. TcpSocket is move-only, so it's
    // wrapped in a shared_ptr to make the enqueued task copyable (required
    // by std::function's target under C++20 — std::move_only_function
    // arrives in C++23).
    while (true) {
        net::TcpSocket client = listener.Accept();

        if (tls_context_.has_value()) {
            try {
                client.UpgradeToTlsServer(*tls_context_);
            } catch (const net::SocketError& e) {
                LogWarn(std::string("TLS handshake failed, dropping connection: ") + e.what());
                continue;
            }
        }

        auto client_ptr = std::make_shared<net::TcpSocket>(std::move(client));
        thread_pool_.Enqueue([this, client_ptr]() { HandleClient(std::move(*client_ptr)); });
    }
}

void Server::HandleClient(net::TcpSocket client) {
    LogInfo("connection accepted");
    while (true) {
        Message request;
        try {
            request = ReceiveMessage(client);
        } catch (const ConnectionClosed&) {
            LogInfo("connection closed");
            return;
        } catch (const ProtocolError& e) {
            LogWarn(std::string("protocol error, dropping connection: ") + e.what());
            return;
        } catch (const net::SocketError& e) {
            LogWarn(std::string("socket error, dropping connection: ") + e.what());
            return;
        }

        LogInfo(std::string("request: ") + OpcodeName(request.opcode));

        try {
            switch (request.opcode) {
                case Opcode::kLogin:
                    HandleLogin(client, request.payload);
                    break;
                case Opcode::kList:
                    HandleList(client, request.payload);
                    break;
                case Opcode::kUpload:
                    HandleUpload(client, request.payload);
                    break;
                case Opcode::kDownload:
                    HandleDownload(client, request.payload);
                    break;
                case Opcode::kDelete:
                    HandleDelete(client, request.payload);
                    break;
                case Opcode::kVerify:
                    HandleVerify(client, request.payload);
                    break;
                case Opcode::kRegisterNode:
                    HandleRegisterNode(client, request.payload);
                    break;
                case Opcode::kNodes:
                    HandleNodes(client, request.payload);
                    break;
                case Opcode::kStatus:
                    HandleStatus(client, request.payload);
                    break;
                default:
                    SendError(client, std::string("command not supported yet: ") +
                                           OpcodeName(request.opcode));
                    break;
            }
        } catch (const std::exception& e) {
            LogError(std::string("handler error: ") + e.what());
            try {
                SendError(client, e.what());
            } catch (const net::SocketError&) {
                return;  // connection is gone; nothing left to do
            }
        }
    }
}

std::optional<std::string> Server::Authorize(const net::TcpSocket& client,
                                              const std::vector<uint8_t>& payload,
                                              size_t& offset) {
    const std::string token = ReadString(payload, offset);
    const auto username = session_manager_.UsernameForToken(token);
    if (!username) {
        SendError(client, "authentication required: run `forgefs login` first");
        return std::nullopt;
    }
    return username;
}

void Server::HandleLogin(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    const std::string username = ReadString(payload, offset);
    const std::string password = ReadString(payload, offset);

    const auto user = user_store_.FindUser(username);
    if (!user || !crypto::VerifyPassword(password, user->password_hash)) {
        SendError(client, "invalid username or password");
        return;
    }

    const std::string token = session_manager_.CreateSession(username);
    LogInfo("login: " + username);

    std::vector<uint8_t> response;
    AppendString(response, token);
    SendMessage(client, Opcode::kOk, response);
}

void Server::HandleList(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    std::vector<uint8_t> response;
    const auto files = chunk_store_.ListFiles();

    AppendUint32(response, static_cast<uint32_t>(files.size()));
    for (const auto& f : files) {
        AppendString(response, f.name);
        AppendUint64(response, f.size);
    }
    SendMessage(client, Opcode::kData, response);
}

void Server::HandleUpload(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    const std::string filename = ReadString(payload, offset);
    const uint64_t total_size = ReadUint64(payload, offset);

    if (!IsSafeFileName(filename)) {
        SendError(client, "invalid file name: " + filename);
        return;
    }

    UploadSession session(chunk_store_, distributor_, filename);
    SendMessage(client, Opcode::kOk);  // ready to receive

    try {
        crypto::Sha256Streamer hasher;
        uint64_t received = 0;
        while (received < total_size) {
            const Message chunk = ReceiveMessage(client);
            if (chunk.opcode != Opcode::kData) {
                throw std::runtime_error(std::string("expected DATA chunk, got ") +
                                          OpcodeName(chunk.opcode));
            }
            session.Write(chunk.payload.data(), chunk.payload.size());
            hasher.Update(chunk.payload.data(), chunk.payload.size());
            received += chunk.payload.size();
        }
        if (received != total_size) {
            throw std::runtime_error("upload size mismatch: expected " + std::to_string(total_size) +
                                      " bytes, received " + std::to_string(received));
        }

        const Message trailer = ReceiveMessage(client);
        if (trailer.opcode != Opcode::kOk) {
            throw std::runtime_error("expected hash trailer after upload data");
        }
        size_t trailer_offset = 0;
        const std::string client_hash = ReadString(trailer.payload, trailer_offset);
        const std::string server_hash = hasher.HexDigest();
        if (client_hash != server_hash) {
            throw std::runtime_error("hash mismatch: upload corrupted in transit (client=" +
                                      client_hash + " server=" + server_hash + ")");
        }

        session.Finish(received, server_hash);
        LogInfo("stored " + filename + " (" + std::to_string(received) +
                " bytes, sha256=" + server_hash + ")");
        SendMessage(client, Opcode::kOk);
    } catch (...) {
        session.Abort();
        throw;
    }
}

void Server::HandleDownload(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    const std::string filename = ReadString(payload, offset);

    if (!IsSafeFileName(filename)) {
        SendError(client, "invalid file name: " + filename);
        return;
    }

    const auto record = chunk_store_.Find(filename);
    if (!record) {
        SendError(client, "file not found: " + filename);
        return;
    }

    std::vector<uint8_t> header_payload;
    AppendUint64(header_payload, record->size);
    SendMessage(client, Opcode::kOk, header_payload);

    crypto::Sha256Streamer hasher;
    for (const auto& chunk_record : record->chunks) {
        std::vector<uint8_t> chunk_data;
        try {
            chunk_data = distributor_.FetchChunk(chunk_record.chunk_id);
        } catch (const std::exception& e) {
            SendError(client, std::string("storage error: ") + e.what());
            return;
        }
        hasher.Update(chunk_data.data(), chunk_data.size());

        // Re-fragment the (up to chunk_size, e.g. 4 MiB) storage chunk into
        // kTransferChunkSize network pieces for the client stream.
        size_t sent_in_chunk = 0;
        while (sent_in_chunk < chunk_data.size()) {
            const size_t take = std::min(kTransferChunkSize, chunk_data.size() - sent_in_chunk);
            std::vector<uint8_t> piece(chunk_data.begin() + static_cast<long>(sent_in_chunk),
                                        chunk_data.begin() + static_cast<long>(sent_in_chunk + take));
            SendMessage(client, Opcode::kData, piece);
            sent_in_chunk += take;
        }
    }

    std::vector<uint8_t> trailer_payload;
    AppendString(trailer_payload, hasher.HexDigest());
    SendMessage(client, Opcode::kOk, trailer_payload);
}

void Server::HandleDelete(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    const std::string filename = ReadString(payload, offset);

    if (!IsSafeFileName(filename)) {
        SendError(client, "invalid file name: " + filename);
        return;
    }
    if (!chunk_store_.Exists(filename)) {
        SendError(client, "file not found: " + filename);
        return;
    }

    const auto orphaned = chunk_store_.DeleteFile(filename);
    distributor_.ReleaseOrphanedChunks(orphaned);
    LogInfo("deleted " + filename);
    SendMessage(client, Opcode::kOk);
}

void Server::HandleVerify(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    const std::string filename = ReadString(payload, offset);

    if (!IsSafeFileName(filename)) {
        SendError(client, "invalid file name: " + filename);
        return;
    }

    const auto record = chunk_store_.Find(filename);
    if (!record) {
        SendError(client, "file not found: " + filename);
        return;
    }

    crypto::Sha256Streamer hasher;
    for (const auto& chunk_record : record->chunks) {
        std::vector<uint8_t> chunk_data;
        try {
            chunk_data = distributor_.FetchChunk(chunk_record.chunk_id);
        } catch (const std::exception& e) {
            SendError(client, std::string("storage error: ") + e.what());
            return;
        }
        hasher.Update(chunk_data.data(), chunk_data.size());
    }

    const std::string computed = hasher.HexDigest();
    const bool matches = computed == record->sha256;

    std::vector<uint8_t> response;
    AppendUint8(response, matches ? 1 : 0);
    AppendString(response, computed);
    AppendString(response, record->sha256);
    SendMessage(client, Opcode::kOk, response);
}

void Server::HandleRegisterNode(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    // Node registration is a separate trust domain from user sessions above
    // and isn't token-authenticated in this phase — see docs/SECURITY.md.
    size_t offset = 0;
    const std::string node_id = ReadString(payload, offset);
    const std::string host = ReadString(payload, offset);
    const uint32_t port = ReadUint32(payload, offset);

    node_registry_.Register(node_id, host, static_cast<uint16_t>(port));
    LogInfo("storage node registered: " + node_id + " (" + host + ":" + std::to_string(port) + ")");
    SendMessage(client, Opcode::kOk);
}

void Server::HandleNodes(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    const auto nodes = node_registry_.AllNodes();

    std::vector<uint8_t> response;
    AppendUint32(response, static_cast<uint32_t>(nodes.size()));
    for (const auto& node : nodes) {
        AppendString(response, node.id);
        AppendString(response, node.host);
        AppendUint32(response, node.port);
        const bool healthy = NodeClient(node.host, node.port).HealthCheck();
        AppendUint8(response, healthy ? 1 : 0);
    }
    SendMessage(client, Opcode::kData, response);
}

void Server::HandleStatus(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    if (!Authorize(client, payload, offset)) return;

    const auto nodes = node_registry_.AllNodes();
    uint32_t healthy_count = 0;
    for (const auto& node : nodes) {
        if (NodeClient(node.host, node.port).HealthCheck()) ++healthy_count;
    }

    const auto files = chunk_store_.ListFiles();
    uint64_t total_bytes = 0;
    for (const auto& f : files) total_bytes += f.size;

    std::vector<uint8_t> response;
    AppendUint32(response, static_cast<uint32_t>(nodes.size()));
    AppendUint32(response, healthy_count);
    AppendUint32(response, static_cast<uint32_t>(files.size()));
    AppendUint64(response, total_bytes);
    SendMessage(client, Opcode::kData, response);
}

}  // namespace forgefs::server
