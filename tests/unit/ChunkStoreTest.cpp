#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "database/ChunkStore.hpp"

using forgefs::database::ChunkRecord;
using forgefs::database::ChunkStore;

namespace {

class ChunkStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("forgefs_chunkstore_test_" + std::to_string(counter_++) + ".db");
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_.string() + "-wal");
        std::filesystem::remove(db_path_.string() + "-shm");
    }

    std::filesystem::path db_path_;
    static inline int counter_ = 0;
};

TEST_F(ChunkStoreTest, CommitAndFindRoundTrip) {
    ChunkStore store(db_path_);

    const std::vector<ChunkRecord> chunks = {
        ChunkRecord{0, "hash1", 100, "hash1"},
        ChunkRecord{1, "hash2", 50, "hash2"},
    };
    const auto orphaned = store.CommitFile("myfile.txt", 150, "filehash", chunks);
    EXPECT_TRUE(orphaned.empty());  // no previous version to orphan

    const auto found = store.Find("myfile.txt");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "myfile.txt");
    EXPECT_EQ(found->size, 150u);
    EXPECT_EQ(found->sha256, "filehash");
    ASSERT_EQ(found->chunks.size(), 2u);
    EXPECT_EQ(found->chunks[0].chunk_id, "hash1");
    EXPECT_EQ(found->chunks[1].chunk_id, "hash2");
}

TEST_F(ChunkStoreTest, FindReturnsNulloptForMissingFile) {
    ChunkStore store(db_path_);
    EXPECT_FALSE(store.Find("nope.txt").has_value());
}

TEST_F(ChunkStoreTest, ExistsReflectsCommittedFiles) {
    ChunkStore store(db_path_);
    EXPECT_FALSE(store.Exists("f.txt"));
    store.CommitFile("f.txt", 10, "h", {ChunkRecord{0, "c1", 10, "c1"}});
    EXPECT_TRUE(store.Exists("f.txt"));
}

TEST_F(ChunkStoreTest, ReuploadOrphansChunksTheNewVersionDoesNotShare) {
    ChunkStore store(db_path_);
    store.CommitFile("f.txt", 10, "h1", {ChunkRecord{0, "old_chunk", 10, "old_chunk"}});

    const auto orphaned =
        store.CommitFile("f.txt", 20, "h2", {ChunkRecord{0, "new_chunk", 20, "new_chunk"}});
    ASSERT_EQ(orphaned.size(), 1u);
    EXPECT_EQ(orphaned[0], "old_chunk");

    const auto found = store.Find("f.txt");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found->chunks.size(), 1u);
    EXPECT_EQ(found->chunks[0].chunk_id, "new_chunk");
}

TEST_F(ChunkStoreTest, ReuploadDoesNotOrphanAChunkTheNewVersionStillUses) {
    ChunkStore store(db_path_);
    store.CommitFile("f.txt", 10, "h1", {ChunkRecord{0, "shared_chunk", 10, "shared_chunk"}});
    const auto orphaned =
        store.CommitFile("f.txt", 10, "h1", {ChunkRecord{0, "shared_chunk", 10, "shared_chunk"}});
    EXPECT_TRUE(orphaned.empty());
}

TEST_F(ChunkStoreTest, DeleteFileReturnsOrphanedChunksAndRemovesTheFile) {
    ChunkStore store(db_path_);
    store.CommitFile("f.txt", 10, "h", {ChunkRecord{0, "only_chunk", 10, "only_chunk"}});

    const auto orphaned = store.DeleteFile("f.txt");
    ASSERT_EQ(orphaned.size(), 1u);
    EXPECT_EQ(orphaned[0], "only_chunk");
    EXPECT_FALSE(store.Exists("f.txt"));
}

TEST_F(ChunkStoreTest, DeleteFileDoesNotOrphanAChunkAnotherFileStillReferences) {
    ChunkStore store(db_path_);
    store.CommitFile("a.txt", 10, "ha", {ChunkRecord{0, "shared", 10, "shared"}});
    store.CommitFile("b.txt", 10, "hb", {ChunkRecord{0, "shared", 10, "shared"}});

    EXPECT_TRUE(store.DeleteFile("a.txt").empty());  // "b.txt" still references "shared"

    const auto orphaned = store.DeleteFile("b.txt");
    ASSERT_EQ(orphaned.size(), 1u);
    EXPECT_EQ(orphaned[0], "shared");
}

TEST_F(ChunkStoreTest, ChunkLocationsAreTrackedAndForgettable) {
    ChunkStore store(db_path_);
    EXPECT_TRUE(store.LocationsFor("chunk1").empty());

    store.RecordChunkLocation("chunk1", "node-a");
    store.RecordChunkLocation("chunk1", "node-b");
    auto locations = store.LocationsFor("chunk1");
    ASSERT_EQ(locations.size(), 2u);
    EXPECT_NE(std::find(locations.begin(), locations.end(), "node-a"), locations.end());
    EXPECT_NE(std::find(locations.begin(), locations.end(), "node-b"), locations.end());

    store.ForgetChunkLocations("chunk1");
    EXPECT_TRUE(store.LocationsFor("chunk1").empty());
}

TEST_F(ChunkStoreTest, ListFilesReturnsEveryCommittedFile) {
    ChunkStore store(db_path_);
    store.CommitFile("a.txt", 1, "ha", {});
    store.CommitFile("b.txt", 2, "hb", {});
    EXPECT_EQ(store.ListFiles().size(), 2u);
}

TEST_F(ChunkStoreTest, AllChunkIdsReflectsCurrentlyReferencedChunksOnly) {
    ChunkStore store(db_path_);
    store.CommitFile("f.txt", 10, "h1", {ChunkRecord{0, "chunk_v1", 10, "chunk_v1"}});
    store.CommitFile("f.txt", 10, "h2", {ChunkRecord{0, "chunk_v2", 10, "chunk_v2"}});

    const auto ids = store.AllChunkIds();
    EXPECT_NE(std::find(ids.begin(), ids.end(), "chunk_v2"), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), "chunk_v1"), ids.end());
}

}  // namespace
