#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rudp {

// Wire format, all fields big-endian:
//
//   0   u16  magic 0x5255
//   2   u8   flags: SYN 1, ACK 2, FIN 4, PROBE 8
//   3   u8   number of SACK blocks (0 to 4)
//   4   u32  seq     sequence number of the first payload byte (SYN and FIN each use one number)
//   8   u32  ack     next sequence number the sender expects to receive
//  12   u32  window  free space in the sender's receive buffer, in bytes
//  16   u16  payload length
//  18   u16  reserved, zero
//  20   u32  timestamp: the sender's clock in microseconds (mod 2^32) when this packet left, plus one (0 means none)
//  24   u32  timestamp echo: the timestamp of the packet that triggered this one
//  28   u32  CRC-32 of the whole packet with this field set to zero
//  32        SACK blocks, 8 bytes each: u32 left edge, u32 right edge (exclusive)
//            then the payload
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kMaxSackBlocks = 4;
constexpr std::uint16_t kMagic = 0x5255;

enum Flag : std::uint8_t { kSyn = 1, kAck = 2, kFin = 4, kProbe = 8 };

struct SackBlock {
  std::uint32_t left = 0;
  std::uint32_t right = 0;  // exclusive
};

struct Header {
  std::uint8_t flags = 0;
  std::uint32_t seq = 0;
  std::uint32_t ack = 0;
  std::uint32_t window = 0;
  std::uint32_t ts_val = 0;
  std::uint32_t ts_ecr = 0;
  std::vector<SackBlock> sack;
};

struct Decoded {
  Header header;
  std::span<const std::uint8_t> payload;  // points into the datagram that was decoded
};

std::uint32_t crc32(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> encode(const Header& h, std::span<const std::uint8_t> payload);

// Returns nothing if the datagram is too short, has the wrong magic, inconsistent lengths or a bad checksum.
std::optional<Decoded> decode(std::span<const std::uint8_t> datagram);

}  // namespace rudp
