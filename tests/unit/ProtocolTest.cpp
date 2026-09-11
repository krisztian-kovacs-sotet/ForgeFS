// Unit tests for the wire-protocol payload codec (protocol/Protocol.cpp).
// Full send/receive-over-a-socket coverage lives in the integration tests
// instead of here — encoding/decoding is the part that's meaningfully
// unit-testable without a live network connection.

#include <gtest/gtest.h>

#include "protocol/Protocol.hpp"

using namespace forgefs::protocol;

TEST(ProtocolCodec, RoundTripsUint8) {
    std::vector<uint8_t> buf;
    AppendUint8(buf, 0xAB);
    size_t offset = 0;
    EXPECT_EQ(ReadUint8(buf, offset), 0xAB);
    EXPECT_EQ(offset, 1u);
}

TEST(ProtocolCodec, RoundTripsUint32) {
    std::vector<uint8_t> buf;
    AppendUint32(buf, 0xDEADBEEF);
    size_t offset = 0;
    EXPECT_EQ(ReadUint32(buf, offset), 0xDEADBEEFu);
    EXPECT_EQ(offset, 4u);
}

TEST(ProtocolCodec, RoundTripsUint64) {
    std::vector<uint8_t> buf;
    AppendUint64(buf, 0x0123456789ABCDEFull);
    size_t offset = 0;
    EXPECT_EQ(ReadUint64(buf, offset), 0x0123456789ABCDEFull);
    EXPECT_EQ(offset, 8u);
}

TEST(ProtocolCodec, RoundTripsString) {
    std::vector<uint8_t> buf;
    AppendString(buf, "hello, forgefs");
    size_t offset = 0;
    EXPECT_EQ(ReadString(buf, offset), "hello, forgefs");
}

TEST(ProtocolCodec, RoundTripsEmptyString) {
    std::vector<uint8_t> buf;
    AppendString(buf, "");
    size_t offset = 0;
    EXPECT_EQ(ReadString(buf, offset), "");
}

TEST(ProtocolCodec, MultipleFieldsInOneBufferReadBackInOrder) {
    std::vector<uint8_t> buf;
    AppendString(buf, "token");
    AppendString(buf, "filename.txt");
    AppendUint64(buf, 12345);

    size_t offset = 0;
    EXPECT_EQ(ReadString(buf, offset), "token");
    EXPECT_EQ(ReadString(buf, offset), "filename.txt");
    EXPECT_EQ(ReadUint64(buf, offset), 12345u);
    EXPECT_EQ(offset, buf.size());
}

TEST(ProtocolCodec, ReadUint32ThrowsOnTruncatedBuffer) {
    std::vector<uint8_t> buf = {1, 2};  // 2 bytes present, 4 needed
    size_t offset = 0;
    EXPECT_THROW(ReadUint32(buf, offset), ProtocolError);
}

TEST(ProtocolCodec, ReadUint64ThrowsOnTruncatedBuffer) {
    std::vector<uint8_t> buf = {1, 2, 3};
    size_t offset = 0;
    EXPECT_THROW(ReadUint64(buf, offset), ProtocolError);
}

TEST(ProtocolCodec, ReadStringThrowsWhenClaimedLengthExceedsBuffer) {
    std::vector<uint8_t> buf;
    AppendUint32(buf, 100);  // claims 100 bytes follow; none do
    size_t offset = 0;
    EXPECT_THROW(ReadString(buf, offset), ProtocolError);
}

TEST(ProtocolCodec, ReadPastEndOfBufferThrows) {
    std::vector<uint8_t> buf;
    AppendUint8(buf, 1);
    size_t offset = 0;
    ReadUint8(buf, offset);
    EXPECT_THROW(ReadUint8(buf, offset), ProtocolError);
}

TEST(OpcodeNameTest, KnownOpcodesHaveReadableNames) {
    EXPECT_STREQ(OpcodeName(Opcode::kUpload), "UPLOAD");
    EXPECT_STREQ(OpcodeName(Opcode::kDownload), "DOWNLOAD");
    EXPECT_STREQ(OpcodeName(Opcode::kOk), "OK");
    EXPECT_STREQ(OpcodeName(Opcode::kError), "ERROR");
    EXPECT_STREQ(OpcodeName(Opcode::kRegisterNode), "REGISTER_NODE");
}
