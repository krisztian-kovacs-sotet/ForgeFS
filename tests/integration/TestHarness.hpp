#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

namespace forgefs::testing {

// Forks and execs `path` with `args`, merging `env` into the child's
// environment. Returns the child pid. Throws std::runtime_error if fork()
// fails; a bad `path` shows up as the child exiting with status 127
// (WaitForPort's caller will time out and the test fails with a clear
// "port never opened" message rather than a silent hang).
pid_t SpawnProcess(const std::string& path, const std::vector<std::string>& args,
                    const std::map<std::string, std::string>& env = {});

// Sends SIGTERM and waits up to `timeout_ms` for the child to exit,
// escalating to SIGKILL if it hasn't. Safe to call with pid <= 0 (no-op) so
// TearDown() can unconditionally stop processes it may or may not have
// already stopped during a test.
void StopProcess(pid_t pid, int timeout_ms = 2000);

// Polls (host, port) with short-timeout connect attempts until one
// succeeds or `timeout_ms` elapses. Returns false on timeout.
bool WaitForPort(const std::string& host, uint16_t port, int timeout_ms = 5000);

}  // namespace forgefs::testing
