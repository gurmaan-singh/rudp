#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rudp/rtt.hpp"

namespace rudp {

struct Endpoint {
  std::uint32_t ip = 0;      // network byte order
  std::uint16_t port = 0;    // network byte order
  bool operator==(const Endpoint&) const = default;
};

// A blocking-with-timeout UDP socket for IPv4, the same on Windows and POSIX.
class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& o) noexcept;
  UdpSocket& operator=(UdpSocket&& o) noexcept;

  // Bind to a local port (0 picks a free one) and return the port that was bound.
  std::uint16_t bind(const std::string& host, std::uint16_t port);
  static Endpoint resolve(const std::string& host, std::uint16_t port);

  void send_to(std::span<const std::uint8_t> data, const Endpoint& to);
  // Wait up to timeout_us for a datagram. Returns nothing on timeout.
  std::optional<std::vector<std::uint8_t>> receive(Time timeout_us, Endpoint* from = nullptr);

 private:
  std::intptr_t handle_;
};

}  // namespace rudp
