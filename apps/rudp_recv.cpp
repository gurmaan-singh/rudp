// Receive one file from rudp_send over real UDP.
//   rudp_recv <port> <output file>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "rudp/connection.hpp"
#include "rudp/udp.hpp"

using namespace rudp;

static Time now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::puts("usage: rudp_recv <port> <output file>");
    return 2;
  }
  UdpSocket sock;
  const std::uint16_t port = sock.bind("0.0.0.0", static_cast<std::uint16_t>(std::atoi(argv[1])));
  std::printf("listening on UDP port %u\n", port);
  std::ofstream out(argv[2], std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }

  Connection c(Role::Server, Config{}, static_cast<std::uint64_t>(now_us()));
  Endpoint peer;
  std::uint64_t total = 0;
  std::uint8_t buf[65536];
  bool closed = false;
  Time linger_until = kNever;
  while (!c.failed()) {
    const Time t = now_us();
    while (std::size_t n = c.read(buf)) {
      out.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(n));
      total += n;
    }
    if (c.eof() && !closed) {
      c.close();
      closed = true;
    }
    if (peer.port != 0) {
      while (auto p = c.poll_transmit(t)) sock.send_to(*p, peer);
    }
    if (c.next_timeout() <= t) c.on_timeout(t);
    if (c.finished() && linger_until == kNever) linger_until = t + 500'000;  // stay a moment in case the last ACK is lost
    if (t >= linger_until) break;
    const Time wait = std::min<Time>(std::max<Time>(c.next_timeout() - t, 0), 2'000);
    Endpoint from;
    if (auto d = sock.receive(wait, &from)) {
      if (peer.port == 0) peer = from;
      c.on_datagram(*d, now_us());
    }
  }
  std::printf("received %llu bytes\n", static_cast<unsigned long long>(total));
  return c.failed() ? 1 : 0;
}
