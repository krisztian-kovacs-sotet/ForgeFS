#include <cstdlib>
#include <iostream>

#include "common/Logging.hpp"
#include "common/PasswordPrompt.hpp"
#include "network/Socket.hpp"
#include "server/Server.hpp"

namespace {

uint16_t ParsePort(const char* env_value, uint16_t default_port) {
    if (env_value == nullptr) return default_port;
    const int parsed = std::atoi(env_value);
    if (parsed <= 0 || parsed > 65535) return default_port;
    return static_cast<uint16_t>(parsed);
}

std::string EnvOr(const char* name, std::string default_value) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::move(default_value);
}

void PrintUsage() {
    std::cerr << "Usage: forgefs_server [--port N] [--data-dir PATH] [--threads N]\n"
                 "                      [--tls-cert PATH --tls-key PATH]\n"
                 "                      [--create-user USERNAME]\n"
                 "\n"
                 "Every flag can also be set via environment variable (flags win if both\n"
                 "are given): FORGEFS_PORT, FORGEFS_DATA_DIR, FORGEFS_THREADS,\n"
                 "FORGEFS_TLS_CERT, FORGEFS_TLS_KEY. FORGEFS_BOOTSTRAP_USER +\n"
                 "FORGEFS_BOOTSTRAP_PASSWORD provision an account non-interactively on\n"
                 "startup (for Docker Compose) instead of exiting like --create-user does.\n";
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = ParsePort(std::getenv("FORGEFS_PORT"), 9000);
    std::string data_dir = EnvOr("FORGEFS_DATA_DIR", "server_data");
    size_t thread_count = forgefs::server::kDefaultThreadCount;
    if (const char* env_threads = std::getenv("FORGEFS_THREADS")) {
        thread_count = static_cast<size_t>(std::atoi(env_threads));
    }
    std::string tls_cert_path = EnvOr("FORGEFS_TLS_CERT", "");
    std::string tls_key_path = EnvOr("FORGEFS_TLS_KEY", "");
    std::string create_user;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            thread_count = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (arg == "--tls-cert" && i + 1 < argc) {
            tls_cert_path = argv[++i];
        } else if (arg == "--tls-key" && i + 1 < argc) {
            tls_key_path = argv[++i];
        } else if (arg == "--create-user" && i + 1 < argc) {
            create_user = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage();
            return 1;
        }
    }

    try {
        forgefs::server::Server server(port, data_dir, thread_count, tls_cert_path, tls_key_path);

        if (!create_user.empty()) {
            // Bootstrap mode: provision one account and exit, rather than
            // exposing account creation as an unauthenticated network
            // endpoint (the roadmap's CLI only has `login`, not `signup`).
            const std::string password = forgefs::common::PromptPassword("Password: ");
            const std::string confirm = forgefs::common::PromptPassword("Confirm password: ");
            if (password != confirm) {
                std::cerr << "Passwords did not match.\n";
                return 1;
            }
            if (!server.CreateUser(create_user, password)) {
                std::cerr << "User '" << create_user << "' already exists.\n";
                return 1;
            }
            std::cout << "Created user '" << create_user << "'.\n";
            return 0;
        }

        // Non-interactive bootstrap for environments (Docker Compose, CI)
        // where nothing can respond to a password prompt. Idempotent: if
        // the account already exists, this is a silent no-op rather than
        // an error, so restarting the container doesn't fail.
        const std::string bootstrap_user = EnvOr("FORGEFS_BOOTSTRAP_USER", "");
        const std::string bootstrap_password = EnvOr("FORGEFS_BOOTSTRAP_PASSWORD", "");
        if (!bootstrap_user.empty() && !bootstrap_password.empty()) {
            if (server.CreateUser(bootstrap_user, bootstrap_password)) {
                forgefs::common::LogInfo("bootstrapped user '" + bootstrap_user + "'");
            }
        }

        server.Run();
    } catch (const forgefs::net::SocketError& e) {
        forgefs::common::LogError(std::string("fatal: ") + e.what());
        return 1;
    } catch (const forgefs::net::TlsError& e) {
        forgefs::common::LogError(std::string("fatal: ") + e.what());
        return 1;
    } catch (const std::exception& e) {
        forgefs::common::LogError(std::string("fatal: ") + e.what());
        return 1;
    }

    return 0;
}
