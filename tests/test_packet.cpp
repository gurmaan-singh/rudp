#include <gtest/gtest.h>

#include <string>

#include "rudp/packet.hpp"
#include "rudp/seq.hpp"

using namespace rudp;

namespace {
std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
}  // namespace

TEST(Crc32, KnownVector) {
  const auto d = bytes("123456789");
  EXPECT_EQ(crc32(d), 0xCBF43926U);  // the standard check value for CRC-32
  EXPECT_EQ(crc32({}), 0U);
}

TEST(Packet, RoundTrip) {
  Header h;
  h.flags = kAck | kFin;
  h.seq = 0xFFFFFFF0U;
  h.ack = 12345;
  h.window = 65536;
  h.sack = {{100, 200}, {300, 400}, {0xFFFFFFFFU, 5}};
  const auto payload = bytes("hello, reliable world");
  const auto wire = encode(h, payload);
  EXPECT_EQ(wire.size(), kHeaderSize + 3 * 8 + payload.size());

  const auto d = decode(wire);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->header.flags, h.flags);
  EXPECT_EQ(d->header.seq, h.seq);
  EXPECT_EQ(d->header.ack, h.ack);
  EXPECT_EQ(d->header.window, h.window);
  ASSERT_EQ(d->header.sack.size(), 3U);
  EXPECT_EQ(d->header.sack[2].left, 0xFFFFFFFFU);
  EXPECT_EQ(std::string(d->payload.begin(), d->payload.end()), "hello, reliable world");
}

TEST(Packet, EmptyPayloadAndNoSack) {
  Header h;
  h.flags = kSyn;
  const auto wire = encode(h, {});
  EXPECT_EQ(wire.size(), kHeaderSize);
  const auto d = decode(wire);
  ASSERT_TRUE(d.has_value());
  EXPECT_TRUE(d->payload.empty());
}

TEST(Packet, AnySingleBitFlipIsDetected) {
  Header h;
  h.flags = kAck;
  h.seq = 77;
  h.sack = {{1, 2}};
  const auto wire = encode(h, bytes("payload bytes"));
  for (std::size_t bit = 0; bit < wire.size() * 8; ++bit) {
    auto bad = wire;
    bad[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
    EXPECT_FALSE(decode(bad).has_value()) << "bit " << bit << " went unnoticed";
  }
}

TEST(Packet, RejectsMalformedDatagrams) {
  EXPECT_FALSE(decode({}).has_value());
  const auto wire = encode(Header{}, bytes("abc"));
  EXPECT_FALSE(decode(std::span(wire).first(kHeaderSize - 1)).has_value());
  EXPECT_FALSE(decode(std::span(wire).first(wire.size() - 1)).has_value());  // truncated payload
  auto longer = wire;
  longer.push_back(0);
  EXPECT_FALSE(decode(longer).has_value());  // trailing garbage
}

TEST(Packet, TooManySackBlocksAreTruncated) {
  Header h;
  for (std::uint32_t i = 0; i < 9; ++i) h.sack.push_back({i * 10, i * 10 + 5});
  const auto d = decode(encode(h, {}));
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->header.sack.size(), kMaxSackBlocks);
}

TEST(Seq, ComparisonsSurviveWraparound) {
  EXPECT_TRUE(seq_lt(5, 10));
  EXPECT_TRUE(seq_lt(0xFFFFFFF0U, 5));   // 5 is "after" a number just below the wrap
  EXPECT_FALSE(seq_lt(5, 0xFFFFFFF0U));
  EXPECT_TRUE(seq_le(7, 7));
  EXPECT_TRUE(seq_gt(3, 0xFFFFFFFEU));
  EXPECT_TRUE(seq_ge(0, 0xFFFFFFFFU));
}
