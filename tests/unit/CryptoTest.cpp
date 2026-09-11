#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "crypto/Hex.hpp"
#include "crypto/PasswordHash.hpp"
#include "crypto/Random.hpp"
#include "crypto/Sha256.hpp"

namespace {

std::string HashBytes(const std::string& s) {
    forgefs::crypto::Sha256Streamer hasher;
    hasher.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return hasher.HexDigest();
}

}  // namespace

TEST(Sha256Test, KnownVectorEmptyString) {
    EXPECT_EQ(HashBytes(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, KnownVectorAbc) {
    EXPECT_EQ(HashBytes("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, IncrementalUpdatesMatchSinglePass) {
    forgefs::crypto::Sha256Streamer incremental;
    const std::string part1 = "hello, ";
    const std::string part2 = "forgefs!";
    incremental.Update(reinterpret_cast<const uint8_t*>(part1.data()), part1.size());
    incremental.Update(reinterpret_cast<const uint8_t*>(part2.data()), part2.size());

    EXPECT_EQ(incremental.HexDigest(), HashBytes(part1 + part2));
}

TEST(Sha256Test, HexOfFileMatchesStreamedHash) {
    const auto path = std::filesystem::temp_directory_path() / "forgefs_sha256_unit_test.txt";
    const std::string content = "file hashing test content";
    {
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    EXPECT_EQ(forgefs::crypto::Sha256HexOfFile(path), HashBytes(content));
    std::filesystem::remove(path);
}

TEST(HexTest, RoundTrips) {
    const std::vector<unsigned char> data = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    const std::string hex = forgefs::crypto::ToHex(data.data(), data.size());
    EXPECT_EQ(hex, "deadbeef0001");
    EXPECT_EQ(forgefs::crypto::FromHex(hex), data);
}

TEST(HexTest, FromHexRejectsOddLength) { EXPECT_TRUE(forgefs::crypto::FromHex("abc").empty()); }

TEST(HexTest, FromHexRejectsNonHexCharacters) { EXPECT_TRUE(forgefs::crypto::FromHex("zzzz").empty()); }

TEST(PasswordHashTest, VerifyAcceptsCorrectPassword) {
    const std::string hash = forgefs::crypto::HashPassword("correct horse battery staple");
    EXPECT_TRUE(forgefs::crypto::VerifyPassword("correct horse battery staple", hash));
}

TEST(PasswordHashTest, VerifyRejectsWrongPassword) {
    const std::string hash = forgefs::crypto::HashPassword("correct horse battery staple");
    EXPECT_FALSE(forgefs::crypto::VerifyPassword("wrong password", hash));
}

TEST(PasswordHashTest, SamePasswordProducesDifferentHashesViaRandomSalt) {
    const std::string hash1 = forgefs::crypto::HashPassword("same password");
    const std::string hash2 = forgefs::crypto::HashPassword("same password");
    EXPECT_NE(hash1, hash2);
    EXPECT_TRUE(forgefs::crypto::VerifyPassword("same password", hash1));
    EXPECT_TRUE(forgefs::crypto::VerifyPassword("same password", hash2));
}

TEST(PasswordHashTest, VerifyRejectsMalformedStoredHash) {
    EXPECT_FALSE(forgefs::crypto::VerifyPassword("anything", "not-a-valid-hash-format"));
    EXPECT_FALSE(forgefs::crypto::VerifyPassword("anything", ""));
}

TEST(RandomTest, TokensHaveExpectedLengthAndAreUnique) {
    const std::string t1 = forgefs::crypto::GenerateRandomToken(32);
    const std::string t2 = forgefs::crypto::GenerateRandomToken(32);
    EXPECT_EQ(t1.size(), 64u);  // 32 bytes -> 64 hex chars
    EXPECT_NE(t1, t2);
}
