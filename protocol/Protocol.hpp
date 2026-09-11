#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "network/Socket.hpp"

namespace forgefs::protocol {

// Opcodes below 100 are client -> server requests; 100+ are server -> client
// (or node -> node) responses. New opcodes are appended, never renumbered,
// so the protocol stays wire-compatible across phases.
//
// UPLOAD/DOWNLOAD stream file bytes as a run of kData messages rather than
// one big payload, so a transfer's memory footprint is O(chunk size) on
// both ends. Each side computes SHA-256 as bytes pass through it, then
// exchanges the digest in a trailer message — no separate read pass needed
// just to know the hash.
//
// Every request below except LOGIN carries a session token as the first
// field of its payload (Phase 8): LOGIN exchanges a username/password for
// the token, and the coordinator rejects any other request whose token
// isn't a currently-valid session with kError before doing anything else.
//
//   LOGIN:    C->S kLogin(username, password)
//             S->C kOk(token) | kError (invalid credentials)
//
//   UPLOAD:   C->S kUpload(token, name, total_size)
//             S->C kOk (ready) | kError
//             C->S kData chunk...             (until total_size bytes sent)
//             C->S kOk (trailer: sha256 hex)
//             S->C kOk (confirmed, hash matched) | kError (mismatch/short)
//
//   DOWNLOAD: C->S kDownload(token, name)
//             S->C kOk(total_size) | kError (not found)
//             S->C kData chunk...             (until total_size bytes sent)
//             S->C kOk (trailer: sha256 hex)
//
//   VERIFY:   C->S kVerify(token, name)
//             S->C kOk(matches: u8, computed sha256 hex, expected sha256 hex) | kError (not found)
//
//   STATUS:   C->S kStatus(token)
//             S->C kData(node_count, healthy_node_count, file_count, total_bytes)
//
//   NODES:    C->S kNodes(token)
//             S->C kData(count, [id, host, port, healthy: u8]...)
//
// The coordinator also speaks a node-protocol, over the same wire format,
// to storage nodes. Node traffic is a separate trust domain from user
// sessions above and isn't token-authenticated in this phase — see
// docs/SECURITY.md for why, and what that means for deployment:
//
//   REGISTER: node->S kRegisterNode(node_id, host, port)
//             S->node kOk | kError
//
//   HEALTH:   S->node kHealthCheck
//             node->S kOk
//
//   STORE:    S->node kStoreChunk(chunk_id, chunk bytes)
//             node->S kOk | kError
//
//   FETCH:    S->node kFetchChunk(chunk_id)
//             node->S kOk(chunk bytes) | kError (not found)
//
//   DELETE:   S->node kDeleteChunk(chunk_id)
//             node->S kOk | kError
enum class Opcode : uint8_t {
    kLogin = 1,
    kList = 2,
    kUpload = 3,
    kDownload = 4,
    kDelete = 5,
    kStatus = 6,
    kNodes = 7,
    kVerify = 8,

    kRegisterNode = 10,
    kHealthCheck = 11,
    kStoreChunk = 12,
    kFetchChunk = 13,
    kDeleteChunk = 14,

    kOk = 100,
    kError = 101,
    kData = 102,
};

const char* OpcodeName(Opcode opcode);

struct Message {
    Opcode opcode;
    std::vector<uint8_t> payload;
};

constexpr uint32_t kProtocolMagic = 0x46474653;  // ASCII "FGFS"
constexpr uint8_t kProtocolVersion = 1;
constexpr uint64_t kMaxPayloadBytes = 512ull * 1024 * 1024;  // 512 MiB safety cap per message

// Upload/download stream file content as a sequence of kData messages this
// large (the last one may be smaller), so neither side ever holds a whole
// file in memory regardless of its size.
constexpr size_t kTransferChunkSize = 64 * 1024;

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& what) : std::runtime_error(what) {}
};

// Thrown by ReceiveMessage when the peer closes the connection cleanly
// before sending a new message header. Not an error condition by itself —
// callers use it to end an accept/read loop.
class ConnectionClosed : public ProtocolError {
public:
    ConnectionClosed() : ProtocolError("connection closed by peer") {}
};

void SendMessage(const net::TcpSocket& socket, Opcode opcode,
                  const std::vector<uint8_t>& payload = {});
Message ReceiveMessage(const net::TcpSocket& socket);

// Convenience: send an kError message carrying a human-readable UTF-8 string.
void SendError(const net::TcpSocket& socket, const std::string& message);

// Payload encoding helpers. Multi-byte integers are big-endian on the wire;
// strings are a uint32 length prefix followed by raw bytes.
void AppendUint8(std::vector<uint8_t>& buf, uint8_t value);
void AppendUint32(std::vector<uint8_t>& buf, uint32_t value);
void AppendUint64(std::vector<uint8_t>& buf, uint64_t value);
void AppendString(std::vector<uint8_t>& buf, const std::string& s);
void AppendBytes(std::vector<uint8_t>& buf, const std::vector<uint8_t>& bytes);

uint8_t ReadUint8(const std::vector<uint8_t>& buf, size_t& offset);
uint32_t ReadUint32(const std::vector<uint8_t>& buf, size_t& offset);
uint64_t ReadUint64(const std::vector<uint8_t>& buf, size_t& offset);
std::string ReadString(const std::vector<uint8_t>& buf, size_t& offset);

}  // namespace forgefs::protocol
