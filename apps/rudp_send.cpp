// Send a file to rudp_recv over real UDP.
//   rudp_send <host> <port> <file>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "rudp/connection.hpp"
#include "rudp/udp.hpp"

using namespace rudp;

static Time now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::puts("usage: rudp_send <host> <port> <file>");
    return 2;
  }
  std::ifstream in(argv[3], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[3]);
    return 1;
  }
  const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  UdpSocket sock;
  sock.bind("0.0.0.0", 0);
  const Endpoint peer = UdpSocket::resolve(argv[1], static_cast<std::uint16_t>(std::atoi(argv[2])));
  Connection c(Role::Client, Config{}, static_cast<std::uint64_t>(now_us()));
  const Time start = now_us();
  c.connect(start);

  std::size_t offset = 0;
  bool closed = false;
  while (!c.finished() && !c.failed()) {
    const Time t = now_us();
    if (offset < data.size()) offset += c.write(std::span(data).subspan(offset));
    else if (!closed) {
      c.close();
      closed = true;
    }
    while (auto p = c.poll_transmit(t)) sock.send_to(*p, peer);
    if (c.next_timeout() <= t) c.on_timeout(t);
    const Time wait = std::min<Time>(std::max<Time>(c.next_timeout() - t, 0), 2'000);
    if (auto d = sock.receive(wait)) c.on_datagram(*d, now_us());
  }
  const double secs = static_cast<double>(now_us() - start) / 1e6;
  if (c.failed()) {
    std::fprintf(stderr, "connection failed\n");
    return 1;
  }
  const Stats& s = c.stats();
  std::printf("sent %zu bytes in %.2f s (%.2f MB/s), %llu retransmissions, srtt %.1f ms\n", data.size(), secs,
              static_cast<double>(data.size()) / secs / 1e6, static_cast<unsigned long long>(s.retransmissions),
              static_cast<double>(c.srtt()) / 1000.0);
  return 0;
}
