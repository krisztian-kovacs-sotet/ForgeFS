// Integration tests: these spawn real forgefs_server/forgefs_storage
// processes (built by the same CMake configure — see
// tests/CMakeLists.txt's FORGEFS_SERVER_BIN/FORGEFS_STORAGE_BIN compile
// definitions) and drive them over the actual network protocol through
// client::Client, the same code path the CLI uses. This is what actually
// exercises "does the distributed system work," as opposed to the unit
// tests, which check individual modules in isolation.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <unistd.h>

#include "client/Client.hpp"
#include "network/Socket.hpp"
#include "tests/integration/TestHarness.hpp"

namespace {

using forgefs::testing::SpawnProcess;
using forgefs::testing::StopProcess;
using forgefs::testing::WaitForPort;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class ClusterTest : public ::testing::Test {
protected:
    void SetUp() override {
        const int id = test_counter_++;
        test_dir_ = std::filesystem::temp_directory_path() /
                    ("forgefs_test_" + std::to_string(::getpid()) + "_" + std::to_string(id));
        std::filesystem::create_directories(test_dir_);

        // Spread ports out per-test so tests could in principle run
        // concurrently without colliding.
        coordinator_port_ = static_cast<uint16_t>(21000 + id * 20);

        coordinator_pid_ =
            SpawnProcess(FORGEFS_SERVER_BIN,
                         {"--port", std::to_string(coordinator_port_), "--data-dir",
                          (test_dir_ / "coordinator").string()},
                         {{"FORGEFS_BOOTSTRAP_USER", "testuser"},
                          {"FORGEFS_BOOTSTRAP_PASSWORD", "testpass"}});
        ASSERT_TRUE(WaitForPort("127.0.0.1", coordinator_port_))
            << "coordinator never started listening";

        for (int i = 0; i < 3; ++i) {
            const auto node_port = static_cast<uint16_t>(coordinator_port_ + 1 + i);
            const pid_t pid = SpawnProcess(
                FORGEFS_STORAGE_BIN,
                {"--port", std::to_string(node_port), "--data-dir",
                 (test_dir_ / ("node" + std::to_string(i))).string(), "--id",
                 "node-" + std::to_string(i), "--register",
                 "127.0.0.1:" + std::to_string(coordinator_port_)});
            ASSERT_TRUE(WaitForPort("127.0.0.1", node_port)) << "storage node " << i
                                                               << " never started listening";
            node_pids_.push_back(pid);
        }

        // Registration is one connect+request the node makes right after it
        // starts listening; give it a beat to land before tests that need
        // all three nodes registered from the first upload.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    void TearDown() override {
        for (auto pid : node_pids_) StopProcess(pid);
        StopProcess(coordinator_pid_);
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    forgefs::client::Client MakeLoggedInClient() {
        forgefs::client::Client client("127.0.0.1", coordinator_port_);
        client.Connect();
        client.Login("testuser", "testpass");
        return client;
    }

    std::filesystem::path test_dir_;
    uint16_t coordinator_port_ = 0;
    pid_t coordinator_pid_ = -1;
    std::vector<pid_t> node_pids_;
    static inline int test_counter_ = 0;
};

TEST_F(ClusterTest, UploadListDownloadRoundTrip) {
    auto client = MakeLoggedInClient();

    const auto local_path = test_dir_ / "upload.txt";
    {
        std::ofstream out(local_path);
        out << "hello forgefs, this is a round-trip test\n";
    }

    const std::string upload_hash = client.Upload(local_path, "roundtrip.txt");
    EXPECT_FALSE(upload_hash.empty());

    const auto files = client.List();
    EXPECT_TRUE(std::any_of(files.begin(), files.end(),
                             [](const auto& f) { return f.name == "roundtrip.txt"; }));

    const auto download_path = test_dir_ / "download.txt";
    const std::string download_hash = client.Download("roundtrip.txt", download_path);
    EXPECT_EQ(upload_hash, download_hash);
    EXPECT_EQ(ReadFile(local_path), ReadFile(download_path));
}

TEST_F(ClusterTest, VerifyReportsMatchOnUncorruptedFile) {
    auto client = MakeLoggedInClient();
    const auto local_path = test_dir_ / "verify_me.txt";
    {
        std::ofstream out(local_path);
        out << "verify this content\n";
    }
    client.Upload(local_path, "verify_me.txt");

    const auto result = client.Verify("verify_me.txt");
    EXPECT_TRUE(result.matches);
    EXPECT_EQ(result.computed_sha256, result.expected_sha256);
}

TEST_F(ClusterTest, DeleteRemovesFileFromListing) {
    auto client = MakeLoggedInClient();
    const auto local_path = test_dir_ / "to_delete.txt";
    {
        std::ofstream out(local_path);
        out << "delete me\n";
    }
    client.Upload(local_path, "to_delete.txt");
    client.Delete("to_delete.txt");

    const auto files = client.List();
    EXPECT_FALSE(std::any_of(files.begin(), files.end(),
                              [](const auto& f) { return f.name == "to_delete.txt"; }));
}

TEST_F(ClusterTest, DownloadOfMissingFileThrows) {
    auto client = MakeLoggedInClient();
    EXPECT_THROW(client.Download("does_not_exist.txt", test_dir_ / "out.txt"),
                 forgefs::client::ClientError);
}

TEST_F(ClusterTest, RequestWithoutLoginIsRejected) {
    forgefs::client::Client client("127.0.0.1", coordinator_port_);
    client.Connect();  // no Login()/SetToken() — token stays empty
    EXPECT_THROW(client.List(), forgefs::client::ClientError);
}

TEST_F(ClusterTest, WrongPasswordRejectsLogin) {
    forgefs::client::Client client("127.0.0.1", coordinator_port_);
    client.Connect();
    EXPECT_THROW(client.Login("testuser", "the-wrong-password"), forgefs::client::ClientError);
}

TEST_F(ClusterTest, SurvivesOneStorageNodeFailure) {
    auto client = MakeLoggedInClient();
    const auto local_path = test_dir_ / "replicated.txt";
    {
        std::ofstream out(local_path);
        out << "this file should survive a storage node going down\n";
    }
    client.Upload(local_path, "replicated.txt");

    StopProcess(node_pids_[0]);
    node_pids_[0] = -1;  // already stopped; TearDown's StopProcess(-1) is a no-op

    const auto download_path = test_dir_ / "replicated_download.txt";
    EXPECT_NO_THROW(client.Download("replicated.txt", download_path));
    EXPECT_EQ(ReadFile(download_path), "this file should survive a storage node going down\n");
}

TEST_F(ClusterTest, RecoversFromACorruptedReplica) {
    auto client = MakeLoggedInClient();
    const auto local_path = test_dir_ / "corrupt_test.txt";
    {
        std::ofstream out(local_path);
        out << "content that will be corrupted on one node's disk\n";
    }
    client.Upload(local_path, "corrupt_test.txt");

    // Simulate on-disk corruption (bit rot, bad sector, ...) by scribbling
    // over every chunk file node-0 is holding, without going through any
    // ForgeFS API.
    for (const auto& entry : std::filesystem::directory_iterator(test_dir_ / "node0")) {
        if (entry.path().extension() != ".chunk") continue;
        std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc);
        corrupt << "CORRUPTED-BYTES-NOT-THE-REAL-CONTENT";
    }

    const auto download_path = test_dir_ / "corrupt_test_download.txt";
    EXPECT_NO_THROW(client.Download("corrupt_test.txt", download_path))
        << "download should fail over to an uncorrupted replica";
    EXPECT_EQ(ReadFile(download_path), "content that will be corrupted on one node's disk\n");
}

TEST_F(ClusterTest, GarbageBytesDontCrashTheCoordinator) {
    {
        auto raw = forgefs::net::TcpSocket::Connect("127.0.0.1", coordinator_port_);
        const std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF};
        raw.SendAll(garbage.data(), garbage.size());
        // `raw` goes out of scope here, closing the connection abruptly
        // mid-message instead of sending a complete (if garbage) frame.
    }

    ASSERT_TRUE(WaitForPort("127.0.0.1", coordinator_port_, 1000))
        << "coordinator stopped accepting connections after malformed input";

    auto client = MakeLoggedInClient();
    EXPECT_NO_THROW(client.List()) << "coordinator should still serve normal requests";
}

}  // namespace
