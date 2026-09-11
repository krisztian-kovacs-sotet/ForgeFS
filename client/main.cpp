// ForgeFS CLI client entry point.
//
// login/list/upload/download/delete/verify/status/nodes are all wired to a
// real TCP connection; upload/download stream in fixed buffers and are
// SHA-256 verified, and files are chunked and replicated across storage
// nodes server-side. Every command but login requires a cached session
// token (client/TokenStore.hpp) obtained via `forgefs login`; optionally
// over TLS via --tls.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "client/Client.hpp"
#include "client/TokenStore.hpp"
#include "common/PasswordPrompt.hpp"
#include "network/Socket.hpp"
#include "network/Tls.hpp"
#include "protocol/Protocol.hpp"

namespace {

constexpr std::string_view kVersion = "1.0.0";

struct ServerAddress {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    bool use_tls = false;
    std::string ca_path;  // empty => no peer verification (self-signed dev cert)
};

ServerAddress ResolveServerAddress(std::vector<std::string>& args) {
    ServerAddress addr;

    if (const char* env = std::getenv("FORGEFS_SERVER")) {
        const std::string value(env);
        const auto colon = value.find(':');
        if (colon != std::string::npos) {
            addr.host = value.substr(0, colon);
            addr.port = static_cast<uint16_t>(std::atoi(value.substr(colon + 1).c_str()));
        } else {
            addr.host = value;
        }
    }

    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--server" && i + 1 < args.size()) {
            const std::string value = args[i + 1];
            const auto colon = value.find(':');
            if (colon != std::string::npos) {
                addr.host = value.substr(0, colon);
                addr.port = static_cast<uint16_t>(std::atoi(value.substr(colon + 1).c_str()));
            } else {
                addr.host = value;
            }
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
        } else if (args[i] == "--tls") {
            addr.use_tls = true;
            args.erase(args.begin() + static_cast<long>(i));
        } else if (args[i] == "--ca" && i + 1 < args.size()) {
            addr.ca_path = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
        } else {
            ++i;
        }
    }

    return addr;
}

void PrintUsage() {
    std::cout <<
        "ForgeFS client " << kVersion << "\n"
        "Usage: forgefs [--server host:port] [--tls [--ca PATH]] <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "  login <username>       Authenticate with the ForgeFS server\n"
        "  list                   List files stored on the server\n"
        "  upload <path>          Upload a local file\n"
        "  download <name>        Download a remote file\n"
        "  delete <name>          Delete a remote file\n"
        "  status                 Show server/cluster status\n"
        "  nodes                  List known storage nodes\n"
        "  verify <name>          Verify integrity of a remote file\n"
        "  --version              Print the client version\n"
        "  --help                 Show this message\n"
        "\n"
        "Server address defaults to 127.0.0.1:9000. Override with --server\n"
        "host:port or the FORGEFS_SERVER environment variable. --tls connects\n"
        "over TLS; --ca PATH verifies the server certificate against a CA file\n"
        "(omit only for a self-signed dev/demo certificate).\n"
        "\n"
        "All commands but `login` require a prior `forgefs login`, which caches\n"
        "a session token at $HOME/.forgefs/token. Set FORGEFS_PASSWORD to log in\n"
        "non-interactively (scripts, Docker Compose) instead of being prompted.\n";
}

int RunCommand(const ServerAddress& addr, const std::string& command,
                const std::vector<std::string>& args) {
    if (command != "list" && command != "upload" && command != "download" &&
        command != "delete" && command != "verify" && command != "status" &&
        command != "nodes" && command != "login") {
        std::cerr << "Unknown command: " << command << "\n\n";
        PrintUsage();
        return 1;
    }
    if ((command == "upload" || command == "download" || command == "delete" ||
         command == "verify") &&
        args.empty()) {
        std::cerr << command << ": missing filename argument\n";
        return 1;
    }
    if (command == "login" && args.empty()) {
        std::cerr << "login: missing <username> argument\n";
        return 1;
    }

    try {
        forgefs::client::Client client(addr.host, addr.port);

        std::optional<forgefs::net::TlsContext> tls_context;
        if (addr.use_tls) {
            tls_context = forgefs::net::TlsContext::ClientContext(addr.ca_path);
            client.ConnectTls(*tls_context);
        } else {
            client.Connect();
        }

        if (command == "login") {
            const std::string username = args[0];
            // FORGEFS_PASSWORD lets scripts/Docker Compose log in
            // non-interactively; otherwise prompt with echo disabled.
            const char* env_password = std::getenv("FORGEFS_PASSWORD");
            const std::string password =
                env_password != nullptr ? std::string(env_password)
                                         : forgefs::common::PromptPassword("Password: ");
            const std::string token = client.Login(username, password);
            forgefs::client::SaveToken(token);
            std::cout << "logged in as " << username << "\n";
            return 0;
        }

        const auto token = forgefs::client::LoadToken();
        if (!token) {
            std::cerr << "Not logged in. Run: forgefs login <username>\n";
            return 1;
        }
        client.SetToken(*token);

        if (command == "list") {
            const auto files = client.List();
            if (files.empty()) {
                std::cout << "(no files)\n";
            }
            for (const auto& f : files) {
                std::cout << f.name << "\t" << f.size << " bytes\n";
            }
        } else if (command == "upload") {
            const std::filesystem::path local_path(args[0]);
            const std::string remote_name =
                args.size() > 1 ? args[1] : local_path.filename().string();

            const auto on_progress = [](uint64_t sent, uint64_t total) {
                const int pct = total == 0 ? 100 : static_cast<int>((sent * 100) / total);
                std::cout << "\r  " << pct << "% (" << sent << "/" << total << " bytes)"
                           << std::flush;
            };
            const std::string hash = client.Upload(local_path, remote_name, on_progress);
            std::cout << "\nuploaded " << local_path.string() << " as " << remote_name
                       << "\n  sha256: " << hash << "\n";
        } else if (command == "download") {
            const std::string remote_name = args[0];
            const std::filesystem::path local_path =
                args.size() > 1 ? std::filesystem::path(args[1])
                                 : std::filesystem::path(remote_name);

            const auto on_progress = [](uint64_t received, uint64_t total) {
                const int pct = total == 0 ? 100 : static_cast<int>((received * 100) / total);
                std::cout << "\r  " << pct << "% (" << received << "/" << total << " bytes)"
                           << std::flush;
            };
            const std::string hash = client.Download(remote_name, local_path, on_progress);
            std::cout << "\ndownloaded " << remote_name << " to " << local_path.string()
                       << "\n  sha256 verified: " << hash << "\n";
        } else if (command == "delete") {
            client.Delete(args[0]);
            std::cout << "deleted " << args[0] << "\n";
        } else if (command == "verify") {
            const auto result = client.Verify(args[0]);
            std::cout << args[0] << "\n  computed: " << result.computed_sha256
                       << "\n  expected: " << result.expected_sha256 << "\n  "
                       << (result.matches ? "OK: file integrity verified"
                                           : "MISMATCH: file may be corrupted")
                       << "\n";
            if (!result.matches) return 1;
        } else if (command == "status") {
            const auto status = client.Status();
            std::cout << "nodes:   " << status.healthy_node_count << "/" << status.node_count
                       << " healthy\n"
                       << "files:   " << status.file_count << "\n"
                       << "bytes:   " << status.total_bytes << "\n";
        } else if (command == "nodes") {
            const auto nodes = client.Nodes();
            if (nodes.empty()) {
                std::cout << "(no storage nodes registered)\n";
            }
            for (const auto& n : nodes) {
                std::cout << n.id << "\t" << n.host << ":" << n.port << "\t"
                           << (n.healthy ? "healthy" : "unreachable") << "\n";
            }
        }
    } catch (const forgefs::client::ClientError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const forgefs::protocol::ProtocolError& e) {
        std::cerr << "protocol error: " << e.what() << "\n";
        return 1;
    } catch (const forgefs::net::TlsError& e) {
        std::cerr << "TLS error: " << e.what() << "\n";
        return 1;
    } catch (const forgefs::net::SocketError& e) {
        std::cerr << "connection error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        PrintUsage();
        return args.empty() ? 1 : 0;
    }

    if (args[0] == "--version") {
        std::cout << "forgefs " << kVersion << "\n";
        return 0;
    }

    const ServerAddress addr = ResolveServerAddress(args);

    if (args.empty()) {
        std::cerr << "Missing command.\n\n";
        PrintUsage();
        return 1;
    }

    const std::string command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    return RunCommand(addr, command, rest);
}
