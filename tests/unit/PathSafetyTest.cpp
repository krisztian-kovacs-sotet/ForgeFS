#include <gtest/gtest.h>

#include "common/PathSafety.hpp"

using forgefs::common::IsSafeFileName;

TEST(PathSafetyTest, AcceptsOrdinaryFilenames) {
    EXPECT_TRUE(IsSafeFileName("photo.jpg"));
    EXPECT_TRUE(IsSafeFileName("my-file_v2.tar.gz"));
}

TEST(PathSafetyTest, RejectsEmptyName) { EXPECT_FALSE(IsSafeFileName("")); }

TEST(PathSafetyTest, RejectsPathTraversal) {
    EXPECT_FALSE(IsSafeFileName(".."));
    EXPECT_FALSE(IsSafeFileName("../etc/passwd"));
    EXPECT_FALSE(IsSafeFileName("a..b"));
}

TEST(PathSafetyTest, RejectsPathSeparators) {
    EXPECT_FALSE(IsSafeFileName("dir/file.txt"));
    EXPECT_FALSE(IsSafeFileName("dir\\file.txt"));
}

TEST(PathSafetyTest, RejectsDotAndDotDot) {
    EXPECT_FALSE(IsSafeFileName("."));
    EXPECT_FALSE(IsSafeFileName(".."));
}

TEST(PathSafetyTest, RejectsOverlongNames) { EXPECT_FALSE(IsSafeFileName(std::string(300, 'a'))); }

TEST(PathSafetyTest, RejectsEmbeddedNulByte) {
    EXPECT_FALSE(IsSafeFileName(std::string("file\0name.txt", 13)));
}
