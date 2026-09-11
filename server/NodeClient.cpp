#include "server/NodeClient.hpp"

#include <chrono>

#include "network/Socket.hpp"
#include "protocol/Protocol.hpp"

namespace forgefs::server {

using forgefs::protocol::AppendString;
using forgefs::protocol::Message;
using forgefs::protocol::Opcode;
using forgefs::protocol::ReadString;
using forgefs::protocol::ReceiveMessage;
using forgefs::protocol::SendMessage;

namespace {

constexpr std::chrono::milliseconds kHealthCheckTimeout{1500};

[[noreturn]] void ThrowFromErrorResponse(const Message& response, const char* what) {
    if (response.opcode == Opcode::kError) {
        size_t offset = 0;
        throw NodeClientError(ReadString(response.payload, offset));
    }
    throw NodeClientError(std::string("unexpected response ") + what);
}

}  // namespace

NodeClient::NodeClient(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

bool NodeClient::HealthCheck() {
    try {
        auto socket = net::TcpSocket::Connect(host_, port_, kHealthCheckTimeout);
        SendMessage(socket, Opcode::kHealthCheck);
        const Message response = ReceiveMessage(socket);
        return response.opcode == Opcode::kOk;
    } catch (const std::exception&) {
        return false;
    }
}

void NodeClient::StoreChunk(const std::string& chunk_id, const std::vector<uint8_t>& data) {
    auto socket = net::TcpSocket::Connect(host_, port_);
    std::vector<uint8_t> payload;
    AppendString(payload, chunk_id);
    payload.insert(payload.end(), data.begin(), data.end());
    SendMessage(socket, Opcode::kStoreChunk, payload);

    const Message response = ReceiveMessage(socket);
    if (response.opcode != Opcode::kOk) ThrowFromErrorResponse(response, "storing chunk");
}

std::vector<uint8_t> NodeClient::FetchChunk(const std::string& chunk_id) {
    auto socket = net::TcpSocket::Connect(host_, port_);
    std::vector<uint8_t> payload;
    AppendString(payload, chunk_id);
    SendMessage(socket, Opcode::kFetchChunk, payload);

    const Message response = ReceiveMessage(socket);
    if (response.opcode != Opcode::kOk) ThrowFromErrorResponse(response, "fetching chunk");
    return response.payload;
}

void NodeClient::DeleteChunk(const std::string& chunk_id) {
    auto socket = net::TcpSocket::Connect(host_, port_);
    std::vector<uint8_t> payload;
    AppendString(payload, chunk_id);
    SendMessage(socket, Opcode::kDeleteChunk, payload);

    const Message response = ReceiveMessage(socket);
    if (response.opcode != Opcode::kOk) ThrowFromErrorResponse(response, "deleting chunk");
}

}  // namespace forgefs::server
