#include "protocol/Protocol.hpp"

#include <arpa/inet.h>
#include <endian.h>

#include <array>
#include <cstring>

namespace forgefs::protocol {

const char* OpcodeName(Opcode opcode) {
    switch (opcode) {
        case Opcode::kLogin: return "LOGIN";
        case Opcode::kList: return "LIST";
        case Opcode::kUpload: return "UPLOAD";
        case Opcode::kDownload: return "DOWNLOAD";
        case Opcode::kDelete: return "DELETE";
        case Opcode::kStatus: return "STATUS";
        case Opcode::kNodes: return "NODES";
        case Opcode::kVerify: return "VERIFY";
        case Opcode::kRegisterNode: return "REGISTER_NODE";
        case Opcode::kHealthCheck: return "HEALTH_CHECK";
        case Opcode::kStoreChunk: return "STORE_CHUNK";
        case Opcode::kFetchChunk: return "FETCH_CHUNK";
        case Opcode::kDeleteChunk: return "DELETE_CHUNK";
        case Opcode::kOk: return "OK";
        case Opcode::kError: return "ERROR";
        case Opcode::kData: return "DATA";
    }
    return "UNKNOWN";
}

void AppendUint8(std::vector<uint8_t>& buf, uint8_t value) { buf.push_back(value); }

void AppendUint32(std::vector<uint8_t>& buf, uint32_t value) {
    const uint32_t be = htonl(value);
    const auto* p = reinterpret_cast<const uint8_t*>(&be);
    buf.insert(buf.end(), p, p + sizeof(be));
}

void AppendUint64(std::vector<uint8_t>& buf, uint64_t value) {
    const uint64_t be = htobe64(value);
    const auto* p = reinterpret_cast<const uint8_t*>(&be);
    buf.insert(buf.end(), p, p + sizeof(be));
}

void AppendString(std::vector<uint8_t>& buf, const std::string& s) {
    AppendUint32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

void AppendBytes(std::vector<uint8_t>& buf, const std::vector<uint8_t>& bytes) {
    buf.insert(buf.end(), bytes.begin(), bytes.end());
}

uint8_t ReadUint8(const std::vector<uint8_t>& buf, size_t& offset) {
    if (offset + sizeof(uint8_t) > buf.size()) {
        throw ProtocolError("truncated payload: expected uint8 at offset " +
                             std::to_string(offset));
    }
    return buf[offset++];
}

uint32_t ReadUint32(const std::vector<uint8_t>& buf, size_t& offset) {
    if (offset + sizeof(uint32_t) > buf.size()) {
        throw ProtocolError("truncated payload: expected uint32 at offset " +
                             std::to_string(offset));
    }
    uint32_t be;
    std::memcpy(&be, buf.data() + offset, sizeof(be));
    offset += sizeof(be);
    return ntohl(be);
}

uint64_t ReadUint64(const std::vector<uint8_t>& buf, size_t& offset) {
    if (offset + sizeof(uint64_t) > buf.size()) {
        throw ProtocolError("truncated payload: expected uint64 at offset " +
                             std::to_string(offset));
    }
    uint64_t be;
    std::memcpy(&be, buf.data() + offset, sizeof(be));
    offset += sizeof(be);
    return be64toh(be);
}

std::string ReadString(const std::vector<uint8_t>& buf, size_t& offset) {
    const uint32_t len = ReadUint32(buf, offset);
    if (offset + len > buf.size()) {
        throw ProtocolError("truncated payload: string length " + std::to_string(len) +
                             " exceeds remaining payload");
    }
    std::string s(reinterpret_cast<const char*>(buf.data() + offset), len);
    offset += len;
    return s;
}

namespace {

// magic(4) + version(1) + opcode(1) + reserved(2) + payload_length(8) = 16 bytes
constexpr std::size_t kHeaderSize = 16;

}  // namespace

void SendMessage(const net::TcpSocket& socket, Opcode opcode,
                  const std::vector<uint8_t>& payload) {
    std::array<uint8_t, kHeaderSize> header{};
    std::size_t off = 0;

    const uint32_t magic_be = htonl(kProtocolMagic);
    std::memcpy(header.data() + off, &magic_be, 4);
    off += 4;

    header[off++] = kProtocolVersion;
    header[off++] = static_cast<uint8_t>(opcode);
    header[off++] = 0;  // reserved
    header[off++] = 0;  // reserved

    const uint64_t len_be = htobe64(static_cast<uint64_t>(payload.size()));
    std::memcpy(header.data() + off, &len_be, 8);
    off += 8;

    socket.SendAll(header.data(), header.size());
    if (!payload.empty()) {
        socket.SendAll(payload.data(), payload.size());
    }
}

Message ReceiveMessage(const net::TcpSocket& socket) {
    std::array<uint8_t, kHeaderSize> header{};
    if (!socket.RecvAll(header.data(), header.size())) {
        throw ConnectionClosed();
    }

    std::size_t off = 0;
    uint32_t magic_be;
    std::memcpy(&magic_be, header.data() + off, 4);
    off += 4;
    const uint32_t magic = ntohl(magic_be);
    if (magic != kProtocolMagic) {
        throw ProtocolError("bad magic number in message header");
    }

    const uint8_t version = header[off++];
    if (version != kProtocolVersion) {
        throw ProtocolError("unsupported protocol version " + std::to_string(version));
    }

    const auto opcode = static_cast<Opcode>(header[off++]);
    off += 2;  // reserved

    uint64_t len_be;
    std::memcpy(&len_be, header.data() + off, 8);
    const uint64_t payload_length = be64toh(len_be);

    if (payload_length > kMaxPayloadBytes) {
        throw ProtocolError("payload length " + std::to_string(payload_length) +
                             " exceeds max of " + std::to_string(kMaxPayloadBytes));
    }

    Message message;
    message.opcode = opcode;
    message.payload.resize(static_cast<std::size_t>(payload_length));
    if (payload_length > 0) {
        if (!socket.RecvAll(message.payload.data(), message.payload.size())) {
            throw ProtocolError("connection closed while reading payload");
        }
    }
    return message;
}

void SendError(const net::TcpSocket& socket, const std::string& message) {
    std::vector<uint8_t> payload;
    AppendString(payload, message);
    SendMessage(socket, Opcode::kError, payload);
}

}  // namespace forgefs::protocol
