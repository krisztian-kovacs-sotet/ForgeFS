#include "client/Client.hpp"

#include <fstream>

#include "crypto/Sha256.hpp"
#include "protocol/Protocol.hpp"

namespace forgefs::client {

using forgefs::protocol::AppendString;
using forgefs::protocol::AppendUint64;
using forgefs::protocol::kTransferChunkSize;
using forgefs::protocol::Message;
using forgefs::protocol::Opcode;
using forgefs::protocol::OpcodeName;
using forgefs::protocol::ReadString;
using forgefs::protocol::ReadUint32;
using forgefs::protocol::ReadUint64;
using forgefs::protocol::ReadUint8;
using forgefs::protocol::ReceiveMessage;
using forgefs::protocol::SendMessage;

namespace {

// Raises a ClientError from an kError response, or a generic ClientError
// if `response` isn't the opcode the caller expected.
void ExpectOpcode(const Message& response, Opcode expected) {
    if (response.opcode == Opcode::kError) {
        size_t offset = 0;
        const std::string message = ReadString(response.payload, offset);
        throw ClientError(message);
    }
    if (response.opcode != expected) {
        throw ClientError("unexpected response from server");
    }
}

}  // namespace

Client::Client(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

void Client::Connect() { socket_ = net::TcpSocket::Connect(host_, port_); }

void Client::ConnectTls(net::TlsContext& tls_context) {
    socket_ = net::TcpSocket::Connect(host_, port_);
    socket_.UpgradeToTlsClient(tls_context, host_);
}

std::string Client::Login(const std::string& username, const std::string& password) {
    std::vector<uint8_t> payload;
    AppendString(payload, username);
    AppendString(payload, password);
    SendMessage(socket_, Opcode::kLogin, payload);

    const Message response = ReceiveMessage(socket_);
    ExpectOpcode(response, Opcode::kOk);

    size_t offset = 0;
    token_ = ReadString(response.payload, offset);
    return token_;
}

std::vector<FileInfo> Client::List() {
    std::vector<uint8_t> payload;
    AppendString(payload, token_);
    SendMessage(socket_, Opcode::kList, payload);
    const Message response = ReceiveMessage(socket_);
    ExpectOpcode(response, Opcode::kData);

    size_t offset = 0;
    const uint32_t count = ReadUint32(response.payload, offset);

    std::vector<FileInfo> files;
    files.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileInfo info;
        info.name = ReadString(response.payload, offset);
        info.size = ReadUint64(response.payload, offset);
        files.push_back(std::move(info));
    }
    return files;
}

std::string Client::Upload(const std::filesystem::path& local_path, const std::string& remote_name,
                            const ProgressCallback& on_progress) {
    std::ifstream in(local_path, std::ios::binary);
    if (!in) {
        throw ClientError("cannot open local file: " + local_path.string());
    }
    const auto total_size = static_cast<uint64_t>(std::filesystem::file_size(local_path));

    std::vector<uint8_t> request_payload;
    AppendString(request_payload, token_);
    AppendString(request_payload, remote_name);
    AppendUint64(request_payload, total_size);
    SendMessage(socket_, Opcode::kUpload, request_payload);

    const Message ready = ReceiveMessage(socket_);
    ExpectOpcode(ready, Opcode::kOk);

    crypto::Sha256Streamer hasher;
    std::vector<uint8_t> buffer(kTransferChunkSize);
    uint64_t sent = 0;
    while (sent < total_size) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;  // defensive: file shrank/changed under us

        std::vector<uint8_t> chunk(buffer.begin(), buffer.begin() + n);
        hasher.Update(chunk.data(), chunk.size());
        SendMessage(socket_, Opcode::kData, chunk);
        sent += static_cast<uint64_t>(n);
        if (on_progress) on_progress(sent, total_size);
    }

    const std::string local_hash = hasher.HexDigest();
    std::vector<uint8_t> trailer_payload;
    AppendString(trailer_payload, local_hash);
    SendMessage(socket_, Opcode::kOk, trailer_payload);

    const Message final_response = ReceiveMessage(socket_);
    ExpectOpcode(final_response, Opcode::kOk);

    return local_hash;
}

std::string Client::Download(const std::string& remote_name, const std::filesystem::path& local_path,
                              const ProgressCallback& on_progress) {
    std::vector<uint8_t> request_payload;
    AppendString(request_payload, token_);
    AppendString(request_payload, remote_name);
    SendMessage(socket_, Opcode::kDownload, request_payload);

    const Message header = ReceiveMessage(socket_);
    ExpectOpcode(header, Opcode::kOk);
    size_t header_offset = 0;
    const uint64_t total_size = ReadUint64(header.payload, header_offset);

    std::ofstream out(local_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw ClientError("cannot open local file for writing: " + local_path.string());
    }

    crypto::Sha256Streamer hasher;
    uint64_t received = 0;
    while (received < total_size) {
        const Message chunk = ReceiveMessage(socket_);
        if (chunk.opcode == Opcode::kError) {
            size_t error_offset = 0;
            throw ClientError(ReadString(chunk.payload, error_offset));
        }
        if (chunk.opcode != Opcode::kData) {
            throw ClientError(std::string("unexpected message during download: ") +
                               OpcodeName(chunk.opcode));
        }
        out.write(reinterpret_cast<const char*>(chunk.payload.data()),
                  static_cast<std::streamsize>(chunk.payload.size()));
        hasher.Update(chunk.payload.data(), chunk.payload.size());
        received += chunk.payload.size();
        if (on_progress) on_progress(received, total_size);
    }
    out.close();

    const Message trailer = ReceiveMessage(socket_);
    ExpectOpcode(trailer, Opcode::kOk);
    size_t trailer_offset = 0;
    const std::string server_hash = ReadString(trailer.payload, trailer_offset);
    const std::string local_hash = hasher.HexDigest();

    if (server_hash != local_hash) {
        throw ClientError("downloaded file failed SHA-256 verification (server=" + server_hash +
                           " local=" + local_hash + ")");
    }

    return local_hash;
}

void Client::Delete(const std::string& remote_name) {
    std::vector<uint8_t> payload;
    AppendString(payload, token_);
    AppendString(payload, remote_name);

    SendMessage(socket_, Opcode::kDelete, payload);
    const Message response = ReceiveMessage(socket_);
    ExpectOpcode(response, Opcode::kOk);
}

VerifyResult Client::Verify(const std::string& remote_name) {
    std::vector<uint8_t> payload;
    AppendString(payload, token_);
    AppendString(payload, remote_name);

    SendMessage(socket_, Opcode::kVerify, payload);
    const Message response = ReceiveMessage(socket_);
    ExpectOpcode(response, Opcode::kOk);

    size_t offset = 0;
    VerifyResult result;
    result.matches = ReadUint8(response.payload, offset) != 0;
    result.computed_sha256 = ReadString(response.payload, offset);
    result.expected_sha256 = ReadString(response.payload, offset);
    return result;
}

std::vector<NodeInfo> Client::Nodes() {
    std::vector<uint8_t> payload;
    AppendString(payload, token_);
    SendMessage(socket_, Opcode::kNodes, payload);
    const Message response = ReceiveMessage(socket_);
    ExpectOpcode(response, Opcode::kData);

    size_t offset = 0;
    const uint32_t count = ReadUint32(response.payload, offset);

    std::vector<NodeInfo> nodes;
    nodes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        NodeInfo node;
        node.id = ReadString(response.payload, offset);
        node.host = ReadString(response.payload, offset);
        node.port = ReadUint32(response.payload, offset);
        node.healthy = ReadUint8(response.payload, offset) != 0;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

StatusInfo Client::Status() {
    std::vector<uint8_t> payload;
    AppendString(payload, token_);
    SendMessage(socket_, Opcode::kStatus, payload);
    const Message response = ReceiveMessage(socket_);
    ExpectOpcode(response, Opcode::kData);

    size_t offset = 0;
    StatusInfo status;
    status.node_count = ReadUint32(response.payload, offset);
    status.healthy_node_count = ReadUint32(response.payload, offset);
    status.file_count = ReadUint32(response.payload, offset);
    status.total_bytes = ReadUint64(response.payload, offset);
    return status;
}

}  // namespace forgefs::client
