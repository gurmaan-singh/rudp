#include "rudp/packet.hpp"

#include <array>

namespace rudp {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}
constexpr auto kTable = make_table();

void put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t get32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data) {
  std::uint32_t c = 0xFFFFFFFFU;
  for (std::uint8_t b : data) c = kTable[(c ^ b) & 0xFFU] ^ (c >> 8);
  return c ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> encode(const Header& h, std::span<const std::uint8_t> payload) {
  const std::size_t nsack = h.sack.size() < kMaxSackBlocks ? h.sack.size() : kMaxSackBlocks;
  std::vector<std::uint8_t> out(kHeaderSize + nsack * 8 + payload.size());
  std::uint8_t* p = out.data();
  put16(p, kMagic);
  p[2] = h.flags;
  p[3] = static_cast<std::uint8_t>(nsack);
  put32(p + 4, h.seq);
  put32(p + 8, h.ack);
  put32(p + 12, h.window);
  put16(p + 16, static_cast<std::uint16_t>(payload.size()));
  put16(p + 18, 0);
  put32(p + 20, h.ts_val);
  put32(p + 24, h.ts_ecr);
  put32(p + 28, 0);
  for (std::size_t i = 0; i < nsack; ++i) {
    put32(p + kHeaderSize + i * 8, h.sack[i].left);
    put32(p + kHeaderSize + i * 8 + 4, h.sack[i].right);
  }
  std::copy(payload.begin(), payload.end(), out.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + nsack * 8));
  put32(p + 28, crc32(out));
  return out;
}

std::optional<Decoded> decode(std::span<const std::uint8_t> d) {
  if (d.size() < kHeaderSize) return std::nullopt;
  const std::uint8_t* p = d.data();
  if (get16(p) != kMagic) return std::nullopt;
  const std::size_t nsack = p[3];
  const std::size_t len = get16(p + 16);
  if (nsack > kMaxSackBlocks || d.size() != kHeaderSize + nsack * 8 + len) return std::nullopt;

  // The checksum covers the packet with its own field zeroed.
  std::vector<std::uint8_t> copy(d.begin(), d.end());
  put32(copy.data() + 28, 0);
  if (crc32(copy) != get32(p + 28)) return std::nullopt;

  Decoded out;
  out.header.flags = p[2];
  out.header.seq = get32(p + 4);
  out.header.ack = get32(p + 8);
  out.header.window = get32(p + 12);
  out.header.ts_val = get32(p + 20);
  out.header.ts_ecr = get32(p + 24);
  for (std::size_t i = 0; i < nsack; ++i) {
    out.header.sack.push_back({get32(p + kHeaderSize + i * 8), get32(p + kHeaderSize + i * 8 + 4)});
  }
  out.payload = d.subspan(kHeaderSize + nsack * 8, len);
  return out;
}

}  // namespace rudp
