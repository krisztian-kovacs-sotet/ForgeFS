// ForgeFS benchmarking tool: measures upload/download throughput and
// latency against a running coordinator, at one or more concurrency
// levels, using the same client::Client code path the CLI uses.
//
// This measures the client-observed side of the wire. Server-side CPU/RSS
// isn't something a client process can see, so pair this with
// benchmarks/monitor_resources.sh sampling the coordinator's PID — see
// docs/BENCHMARKS.md.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "client/Client.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    std::string username;
    std::string password;
    uint64_t size = 1024 * 1024;  // 1 MiB default payload
    uint32_t count = 20;          // operations per run
    uint32_t concurrency = 1;
    std::vector<uint32_t> levels = {1, 2, 4, 8};  // for `sweep`
};

std::string FormatBytes(uint64_t bytes) {
    if (bytes >= 1024ull * 1024) return std::to_string(bytes / (1024 * 1024)) + " MiB";
    if (bytes >= 1024) return std::to_string(bytes / 1024) + " KiB";
    return std::to_string(bytes) + " B";
}

std::filesystem::path WriteRandomPayload(uint64_t size) {
    std::vector<uint8_t> data(size);
    std::mt19937 rng(42);  // fixed seed: reproducible runs, not a security concern here
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& byte : data) byte = static_cast<uint8_t>(dist(rng));

    const auto path = std::filesystem::temp_directory_path() / "forgefs_benchmark_payload.bin";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

struct Stats {
    std::vector<double> latencies_ms;

    void Print(const std::string& label, uint64_t total_bytes, double wall_seconds) const {
        if (latencies_ms.empty()) {
            std::cout << label << ": no samples\n";
            return;
        }
        std::vector<double> sorted = latencies_ms;
        std::sort(sorted.begin(), sorted.end());

        const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        const double avg = sum / static_cast<double>(sorted.size());
        auto percentile = [&](double p) {
            const auto idx = std::min(sorted.size() - 1,
                                       static_cast<size_t>(p * static_cast<double>(sorted.size())));
            return sorted[idx];
        };

        std::cout << "== " << label << " ==\n"
                   << "  operations:  " << sorted.size() << "\n"
                   << "  wall time:   " << wall_seconds << " s\n";
        if (total_bytes > 0 && wall_seconds > 0) {
            const double mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
            std::cout << "  throughput:  " << (mb / wall_seconds) << " MB/s (" << mb
                       << " MB total)\n";
        }
        std::cout << "  latency ms:  min=" << sorted.front() << " avg=" << avg
                   << " p50=" << percentile(0.50) << " p95=" << percentile(0.95)
                   << " p99=" << percentile(0.99) << " max=" << sorted.back() << "\n";
    }
};

forgefs::client::Client LoggedInClient(const Options& opts) {
    forgefs::client::Client client(opts.host, opts.port);
    client.Connect();
    client.Login(opts.username, opts.password);
    return client;
}

// Splits `count` operations as evenly as possible across `concurrency`
// worker threads, each on its own logged-in Client (Client is one
// connection, used by one thread — not shared), timing each operation
// individually. `name_for(worker, i)` generates each operation's remote
// filename so concurrent workers never collide.
Stats RunConcurrentOp(const Options& opts, uint32_t concurrency, uint32_t count,
                       const std::function<std::string(uint32_t, uint32_t)>& name_for,
                       const std::function<void(forgefs::client::Client&, const std::string&)>& op) {
    Stats combined;
    std::mutex mutex;

    std::vector<std::thread> workers;
    for (uint32_t w = 0; w < concurrency; ++w) {
        const uint32_t ops = count / concurrency + (w < count % concurrency ? 1 : 0);
        workers.emplace_back([&, w, ops]() {
            auto client = LoggedInClient(opts);
            std::vector<double> local;
            local.reserve(ops);
            for (uint32_t i = 0; i < ops; ++i) {
                const std::string name = name_for(w, i);
                const auto start = Clock::now();
                op(client, name);
                const auto end = Clock::now();
                local.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            }
            std::lock_guard<std::mutex> lock(mutex);
            combined.latencies_ms.insert(combined.latencies_ms.end(), local.begin(), local.end());
        });
    }
    for (auto& t : workers) t.join();
    return combined;
}

void CmdUpload(const Options& opts) {
    const auto payload_path = WriteRandomPayload(opts.size);

    const auto wall_start = Clock::now();
    const auto stats = RunConcurrentOp(
        opts, opts.concurrency, opts.count,
        [](uint32_t w, uint32_t i) {
            return "bench_up_" + std::to_string(w) + "_" + std::to_string(i) + ".bin";
        },
        [&](forgefs::client::Client& client, const std::string& name) {
            client.Upload(payload_path, name);
        });
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();

    stats.Print("upload (size=" + FormatBytes(opts.size) +
                    ", concurrency=" + std::to_string(opts.concurrency) + ")",
                opts.size * stats.latencies_ms.size(), wall_seconds);
    std::filesystem::remove(payload_path);
}

void CmdDownload(const Options& opts) {
    const auto payload_path = WriteRandomPayload(opts.size);

    // Seed the files first — untimed, this is setup, not the measurement.
    RunConcurrentOp(
        opts, opts.concurrency, opts.count,
        [](uint32_t w, uint32_t i) {
            return "bench_dl_" + std::to_string(w) + "_" + std::to_string(i) + ".bin";
        },
        [&](forgefs::client::Client& client, const std::string& name) {
            client.Upload(payload_path, name);
        });

    const auto download_dir = std::filesystem::temp_directory_path() / "forgefs_benchmark_downloads";
    std::filesystem::create_directories(download_dir);

    const auto wall_start = Clock::now();
    const auto stats = RunConcurrentOp(
        opts, opts.concurrency, opts.count,
        [](uint32_t w, uint32_t i) {
            return "bench_dl_" + std::to_string(w) + "_" + std::to_string(i) + ".bin";
        },
        [&](forgefs::client::Client& client, const std::string& name) {
            client.Download(name, download_dir / name);
        });
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();

    stats.Print("download (size=" + FormatBytes(opts.size) +
                    ", concurrency=" + std::to_string(opts.concurrency) + ")",
                opts.size * stats.latencies_ms.size(), wall_seconds);

    std::filesystem::remove(payload_path);
    std::filesystem::remove_all(download_dir);
}

void CmdLatency(const Options& opts) {
    const auto wall_start = Clock::now();
    const auto stats = RunConcurrentOp(
        opts, 1, opts.count, [](uint32_t, uint32_t i) { return "probe_" + std::to_string(i); },
        [&](forgefs::client::Client& client, const std::string&) { client.List(); });
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();
    stats.Print("latency (LIST x" + std::to_string(opts.count) + ", sequential)", 0, wall_seconds);
}

void CmdSweep(const Options& opts) {
    std::cout << "concurrency\tthroughput_MBps\tavg_ms\tp95_ms\n";
    for (uint32_t level : opts.levels) {
        Options level_opts = opts;
        level_opts.concurrency = level;
        const auto payload_path = WriteRandomPayload(opts.size);

        const auto wall_start = Clock::now();
        const auto stats = RunConcurrentOp(
            level_opts, level, opts.count,
            [level](uint32_t w, uint32_t i) {
                return "sweep_" + std::to_string(level) + "_" + std::to_string(w) + "_" +
                       std::to_string(i) + ".bin";
            },
            [&](forgefs::client::Client& client, const std::string& name) {
                client.Upload(payload_path, name);
            });
        const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();

        std::vector<double> sorted = stats.latencies_ms;
        std::sort(sorted.begin(), sorted.end());
        const double avg =
            sorted.empty() ? 0.0
                            : std::accumulate(sorted.begin(), sorted.end(), 0.0) /
                                  static_cast<double>(sorted.size());
        const double p95 = sorted.empty() ? 0.0
                                           : sorted[std::min(sorted.size() - 1,
                                                              static_cast<size_t>(0.95 * sorted.size()))];
        const double mb = static_cast<double>(opts.size) * static_cast<double>(sorted.size()) /
                           (1024.0 * 1024.0);

        std::cout << level << "\t" << (wall_seconds > 0 ? mb / wall_seconds : 0.0) << "\t" << avg
                   << "\t" << p95 << "\n";
        std::filesystem::remove(payload_path);
    }
}

void PrintUsage() {
    std::cerr <<
        "Usage: forgefs_benchmark <upload|download|latency|sweep> --username U [options]\n"
        "\n"
        "  --server host:port    Coordinator address (default 127.0.0.1:9000)\n"
        "  --username U          Required: an existing account\n"
        "  --password P          Or set FORGEFS_PASSWORD\n"
        "  --size BYTES          Payload size per operation (default 1048576)\n"
        "  --count N             Operations per run (default 20)\n"
        "  --concurrency N       Parallel client connections (default 1;\n"
        "                        ignored by `latency`, which is always 1)\n"
        "  --levels A,B,C        Concurrency levels for `sweep` (default 1,2,4,8)\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[1];
    if (command != "upload" && command != "download" && command != "latency" &&
        command != "sweep") {
        std::cerr << "Unknown command: " << command << "\n\n";
        PrintUsage();
        return 1;
    }

    Options opts;
    if (const char* env_password = std::getenv("FORGEFS_PASSWORD")) opts.password = env_password;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            const std::string value = argv[++i];
            const auto colon = value.find(':');
            if (colon != std::string::npos) {
                opts.host = value.substr(0, colon);
                opts.port = static_cast<uint16_t>(std::atoi(value.substr(colon + 1).c_str()));
            } else {
                opts.host = value;
            }
        } else if (arg == "--username" && i + 1 < argc) {
            opts.username = argv[++i];
        } else if (arg == "--password" && i + 1 < argc) {
            opts.password = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            opts.size = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--count" && i + 1 < argc) {
            opts.count = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--concurrency" && i + 1 < argc) {
            opts.concurrency = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--levels" && i + 1 < argc) {
            opts.levels.clear();
            std::stringstream ss(argv[++i]);
            std::string part;
            while (std::getline(ss, part, ',')) {
                opts.levels.push_back(static_cast<uint32_t>(std::atoi(part.c_str())));
            }
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage();
            return 1;
        }
    }

    if (opts.username.empty() || opts.password.empty()) {
        std::cerr << "--username and --password (or FORGEFS_PASSWORD) are required\n";
        return 1;
    }
    if (opts.concurrency == 0) opts.concurrency = 1;
    if (opts.count == 0) opts.count = 1;

    try {
        if (command == "upload") {
            CmdUpload(opts);
        } else if (command == "download") {
            CmdDownload(opts);
        } else if (command == "latency") {
            CmdLatency(opts);
        } else if (command == "sweep") {
            CmdSweep(opts);
        }
    } catch (const std::exception& e) {
        std::cerr << "benchmark failed: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
