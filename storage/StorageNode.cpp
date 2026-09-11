#include "storage/StorageNode.hpp"

#include <fstream>
#include <memory>
#include <system_error>

#include "common/Logging.hpp"
#include "common/PathSafety.hpp"
#include "protocol/Protocol.hpp"

namespace forgefs::storage {

using forgefs::common::IsSafeFileName;
using forgefs::common::LogError;
using forgefs::common::LogInfo;
using forgefs::common::LogWarn;
using forgefs::protocol::ConnectionClosed;
using forgefs::protocol::Message;
using forgefs::protocol::Opcode;
using forgefs::protocol::OpcodeName;
using forgefs::protocol::ProtocolError;
using forgefs::protocol::ReadString;
using forgefs::protocol::ReceiveMessage;
using forgefs::protocol::SendError;
using forgefs::protocol::SendMessage;

StorageNode::StorageNode(uint16_t port, std::filesystem::path data_dir, size_t thread_count)
    : port_(port), data_dir_(std::move(data_dir)), thread_pool_(thread_count) {
    std::filesystem::create_directories(data_dir_);
}

std::filesystem::path StorageNode::ChunkPath(const std::string& chunk_id) const {
    return data_dir_ / (chunk_id + ".chunk");
}

void StorageNode::Run() {
    net::TcpSocket listener = net::TcpSocket::Listen(port_);
    LogInfo("ForgeFS storage node listening on port " + std::to_string(port_) +
            ", data dir: " + data_dir_.string());

    while (true) {
        net::TcpSocket client = listener.Accept();
        auto client_ptr = std::make_shared<net::TcpSocket>(std::move(client));
        thread_pool_.Enqueue([this, client_ptr]() { HandleClient(std::move(*client_ptr)); });
    }
}

void StorageNode::HandleClient(net::TcpSocket client) {
    while (true) {
        Message request;
        try {
            request = ReceiveMessage(client);
        } catch (const ConnectionClosed&) {
            return;
        } catch (const ProtocolError& e) {
            LogWarn(std::string("protocol error, dropping connection: ") + e.what());
            return;
        } catch (const net::SocketError& e) {
            LogWarn(std::string("socket error, dropping connection: ") + e.what());
            return;
        }

        try {
            switch (request.opcode) {
                case Opcode::kHealthCheck:
                    HandleHealthCheck(client);
                    break;
                case Opcode::kStoreChunk:
                    HandleStoreChunk(client, request.payload);
                    break;
                case Opcode::kFetchChunk:
                    HandleFetchChunk(client, request.payload);
                    break;
                case Opcode::kDeleteChunk:
                    HandleDeleteChunk(client, request.payload);
                    break;
                default:
                    SendError(client, std::string("unsupported request: ") +
                                           OpcodeName(request.opcode));
                    break;
            }
        } catch (const std::exception& e) {
            LogError(std::string("handler error: ") + e.what());
            try {
                SendError(client, e.what());
            } catch (const net::SocketError&) {
                return;
            }
        }
    }
}

void StorageNode::HandleHealthCheck(const net::TcpSocket& client) { SendMessage(client, Opcode::kOk); }

void StorageNode::HandleStoreChunk(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    const std::string chunk_id = ReadString(payload, offset);
    if (!IsSafeFileName(chunk_id)) {
        SendError(client, "invalid chunk id");
        return;
    }

    const auto path = ChunkPath(chunk_id);
    if (!std::filesystem::exists(path)) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            SendError(client, "failed to write chunk: " + chunk_id);
            return;
        }
        out.write(reinterpret_cast<const char*>(payload.data() + offset),
                  static_cast<std::streamsize>(payload.size() - offset));
    }
    // else: identical content already stored under this id — idempotent no-op.

    SendMessage(client, Opcode::kOk);
}

void StorageNode::HandleFetchChunk(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    const std::string chunk_id = ReadString(payload, offset);
    if (!IsSafeFileName(chunk_id)) {
        SendError(client, "invalid chunk id");
        return;
    }

    const auto path = ChunkPath(chunk_id);
    if (!std::filesystem::exists(path)) {
        SendError(client, "chunk not found: " + chunk_id);
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        SendError(client, "failed to read chunk: " + chunk_id);
        return;
    }
    const auto size = std::filesystem::file_size(path);
    std::vector<uint8_t> data(size);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

    SendMessage(client, Opcode::kOk, data);
}

void StorageNode::HandleDeleteChunk(const net::TcpSocket& client, const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    const std::string chunk_id = ReadString(payload, offset);
    if (!IsSafeFileName(chunk_id)) {
        SendError(client, "invalid chunk id");
        return;
    }

    std::error_code ec;
    std::filesystem::remove(ChunkPath(chunk_id), ec);
    SendMessage(client, Opcode::kOk);
}

}  // namespace forgefs::storage
