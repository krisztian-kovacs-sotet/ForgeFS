#include "tests/integration/TestHarness.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "network/Socket.hpp"

namespace forgefs::testing {

pid_t SpawnProcess(const std::string& path, const std::vector<std::string>& args,
                    const std::map<std::string, std::string>& env) {
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        // Child process.
        for (const auto& [key, value] : env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(path.c_str()));
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        execv(path.c_str(), argv.data());
        _exit(127);  // execv only returns on failure
    }

    return pid;  // parent process
}

void StopProcess(pid_t pid, int timeout_ms) {
    if (pid <= 0) return;

    ::kill(pid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

bool WaitForPort(const std::string& host, uint16_t port, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            net::TcpSocket::Connect(host, port, std::chrono::milliseconds(200));
            return true;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return false;
}

}  // namespace forgefs::testing
