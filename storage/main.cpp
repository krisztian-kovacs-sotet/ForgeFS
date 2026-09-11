#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/Logging.hpp"
#include "network/Socket.hpp"
#include "protocol/Protocol.hpp"
#include "storage/StorageNode.hpp"

namespace {

std::string EnvOr(const char* name, std::string default_value) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::move(default_value);
}

void RegisterWithCoordinator(const std::string& coordinator_host, uint16_t coordinator_port,
                              const std::string& node_id, const std::string& advertise_host,
                              uint16_t advertise_port) {
    using namespace forgefs;

    auto socket = net::TcpSocket::Connect(coordinator_host, coordinator_port);
    std::vector<uint8_t> payload;
    protocol::AppendString(payload, node_id);
    protocol::AppendString(payload, advertise_host);
    protocol::AppendUint32(payload, advertise_port);
    protocol::SendMessage(socket, protocol::Opcode::kRegisterNode, payload);

    const protocol::Message response = protocol::ReceiveMessage(socket);
    if (response.opcode != protocol::Opcode::kOk) {
        size_t offset = 0;
        const std::string message = response.opcode == protocol::Opcode::kError
                                         ? protocol::ReadString(response.payload, offset)
                                         : "unexpected response";
        throw std::runtime_error("registration failed: " + message);
    }
}

// Retries registration for a while rather than failing immediately, since
// under Docker Compose (or any orchestrator without startup-order
// guarantees) this node's container can easily start before the
// coordinator is ready to accept connections.
bool RegisterWithRetry(const std::string& coordinator_host, uint16_t coordinator_port,
                        const std::string& node_id, const std::string& advertise_host,
                        uint16_t advertise_port) {
    constexpr int kMaxAttempts = 30;
    constexpr auto kRetryDelay = std::chrono::seconds(2);

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        try {
            RegisterWithCoordinator(coordinator_host, coordinator_port, node_id, advertise_host,
                                     advertise_port);
            return true;
        } catch (const std::exception& e) {
            forgefs::common::LogWarn("registration attempt " + std::to_string(attempt) + "/" +
                                      std::to_string(kMaxAttempts) + " failed: " + e.what());
            if (attempt < kMaxAttempts) std::this_thread::sleep_for(kRetryDelay);
        }
    }
    return false;
}

void PrintUsage() {
    std::cerr << "Usage: forgefs_storage [--port N] [--data-dir PATH] [--id ID] [--threads N]\n"
                 "                       [--advertise-host HOST] [--advertise-port N]\n"
                 "                       [--register COORDINATOR_HOST:PORT]\n"
                 "\n"
                 "Every flag can also be set via environment variable (flags win if both\n"
                 "are given): FORGEFS_PORT, FORGEFS_DATA_DIR, FORGEFS_ID, FORGEFS_THREADS,\n"
                 "FORGEFS_ADVERTISE_HOST, FORGEFS_ADVERTISE_PORT, FORGEFS_REGISTER\n"
                 "(host:port).\n";
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 9100;
    if (const char* env_port = std::getenv("FORGEFS_PORT")) port = static_cast<uint16_t>(std::atoi(env_port));

    std::string data_dir = EnvOr("FORGEFS_DATA_DIR", "node_data");
    std::string node_id = EnvOr("FORGEFS_ID", "node-" + std::to_string(::getpid()));
    std::string advertise_host = EnvOr("FORGEFS_ADVERTISE_HOST", "127.0.0.1");
    uint16_t advertise_port = 0;  // 0 => defaults to `port` once parsing is done
    if (const char* env_adv_port = std::getenv("FORGEFS_ADVERTISE_PORT")) {
        advertise_port = static_cast<uint16_t>(std::atoi(env_adv_port));
    }
    std::string coordinator_host;
    uint16_t coordinator_port = 0;
    if (const char* env_register = std::getenv("FORGEFS_REGISTER")) {
        const std::string value(env_register);
        const auto colon = value.find(':');
        if (colon != std::string::npos) {
            coordinator_host = value.substr(0, colon);
            coordinator_port = static_cast<uint16_t>(std::atoi(value.substr(colon + 1).c_str()));
        }
    }
    size_t thread_count = forgefs::storage::kDefaultThreadCount;
    if (const char* env_threads = std::getenv("FORGEFS_THREADS")) {
        thread_count = static_cast<size_t>(std::atoi(env_threads));
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--id" && i + 1 < argc) {
            node_id = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            thread_count = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (arg == "--advertise-host" && i + 1 < argc) {
            advertise_host = argv[++i];
        } else if (arg == "--advertise-port" && i + 1 < argc) {
            advertise_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--register" && i + 1 < argc) {
            const std::string value = argv[++i];
            const auto colon = value.find(':');
            if (colon == std::string::npos) {
                std::cerr << "--register expects host:port\n";
                return 1;
            }
            coordinator_host = value.substr(0, colon);
            coordinator_port = static_cast<uint16_t>(std::atoi(value.substr(colon + 1).c_str()));
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage();
            return 1;
        }
    }
    if (advertise_port == 0) advertise_port = port;

    if (!coordinator_host.empty()) {
        if (!RegisterWithRetry(coordinator_host, coordinator_port, node_id, advertise_host,
                                advertise_port)) {
            forgefs::common::LogError("failed to register with coordinator at " + coordinator_host +
                                       ":" + std::to_string(coordinator_port) + " after retrying");
            return 1;
        }
        forgefs::common::LogInfo("registered with coordinator at " + coordinator_host + ":" +
                                  std::to_string(coordinator_port) + " as '" + node_id + "' (" +
                                  advertise_host + ":" + std::to_string(advertise_port) + ")");
    }

    try {
        forgefs::storage::StorageNode node(port, data_dir, thread_count);
        node.Run();
    } catch (const forgefs::net::SocketError& e) {
        forgefs::common::LogError(std::string("fatal: ") + e.what());
        return 1;
    }

    return 0;
}
