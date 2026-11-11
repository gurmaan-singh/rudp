#include "rudp/udp.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace rudp {

namespace {

#ifdef _WIN32
struct WinsockInit {
  WinsockInit() {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) throw std::runtime_error("WSAStartup failed");
  }
  ~WinsockInit() { WSACleanup(); }
};
void ensure_init() { static WinsockInit init; }
using Native = SOCKET;
constexpr Native kInvalid = INVALID_SOCKET;
void close_native(Native s) { closesocket(s); }
std::string last_error() { return "error " + std::to_string(WSAGetLastError()); }
#else
void ensure_init() {}
using Native = int;
constexpr Native kInvalid = -1;
void close_native(Native s) { ::close(s); }
std::string last_error() { return std::strerror(errno); }
#endif

Native native(std::intptr_t h) { return static_cast<Native>(h); }

}  // namespace

UdpSocket::UdpSocket() {
  ensure_init();
  const Native s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalid) throw std::runtime_error("socket(): " + last_error());
  handle_ = static_cast<std::intptr_t>(s);
}

UdpSocket::~UdpSocket() {
  if (handle_ != -1) close_native(native(handle_));
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : handle_(std::exchange(o.handle_, -1)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
  if (this != &o) {
    if (handle_ != -1) close_native(native(handle_));
    handle_ = std::exchange(o.handle_, -1);
  }
  return *this;
}

Endpoint UdpSocket::resolve(const std::string& host, std::uint16_t port) {
  ensure_init();
  in_addr a{};
  if (inet_pton(AF_INET, host.c_str(), &a) != 1) throw std::invalid_argument("not an IPv4 address: " + host);
  Endpoint e;
  e.ip = a.s_addr;
  e.port = htons(port);
  return e;
}

std::uint16_t UdpSocket::bind(const std::string& host, std::uint16_t port) {
  const Endpoint e = resolve(host, port);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = e.ip;
  a.sin_port = e.port;
  if (::bind(native(handle_), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) throw std::runtime_error("bind(): " + last_error());
  socklen_t len = sizeof a;
  if (::getsockname(native(handle_), reinterpret_cast<sockaddr*>(&a), &len) != 0) throw std::runtime_error("getsockname(): " + last_error());
  return ntohs(a.sin_port);
}

void UdpSocket::send_to(std::span<const std::uint8_t> data, const Endpoint& to) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = to.ip;
  a.sin_port = to.port;
  // A failed send on UDP is just a lost packet, which the protocol already tolerates.
  (void)::sendto(native(handle_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                 reinterpret_cast<sockaddr*>(&a), sizeof a);
}

std::optional<std::vector<std::uint8_t>> UdpSocket::receive(Time timeout_us, Endpoint* from) {
  const int ms = timeout_us <= 0 ? 0 : static_cast<int>(std::min<Time>((timeout_us + 999) / 1000, 1'000'000));
#ifdef _WIN32
  WSAPOLLFD pfd{native(handle_), POLLIN, 0};
  const int r = WSAPoll(&pfd, 1, ms);
#else
  pollfd pfd{native(handle_), POLLIN, 0};
  const int r = ::poll(&pfd, 1, ms);
#endif
  if (r <= 0) return std::nullopt;
  std::vector<std::uint8_t> buf(65536);
  sockaddr_in a{};
  socklen_t len = sizeof a;
  const int n = ::recvfrom(native(handle_), reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&a), &len);
  if (n < 0) return std::nullopt;
  buf.resize(static_cast<std::size_t>(n));
  if (from) {
    from->ip = a.sin_addr.s_addr;
    from->port = a.sin_port;
  }
  return buf;
}

}  // namespace rudp
