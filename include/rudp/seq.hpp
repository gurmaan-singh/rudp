#pragma once
#include <cstdint>

namespace rudp {

// Sequence numbers are 32 bits and wrap. Compare them as signed differences, which is
// correct as long as two numbers being compared are less than 2^31 apart.
inline bool seq_lt(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) < 0; }
inline bool seq_le(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) <= 0; }
inline bool seq_gt(std::uint32_t a, std::uint32_t b) { return seq_lt(b, a); }
inline bool seq_ge(std::uint32_t a, std::uint32_t b) { return seq_le(b, a); }

struct SeqLess {
  bool operator()(std::uint32_t a, std::uint32_t b) const { return seq_lt(a, b); }
};

}  // namespace rudp
