#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "rudp/connection.hpp"
#include "rudp/udp.hpp"

using namespace rudp;

namespace {

Time now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Drive one connection over a real UDP socket until `done` says to stop.
template <class App>
void pump(Connection& c, UdpSocket& sock, Endpoint peer, bool learn_peer, App&& app, std::atomic<bool>& stop) {
  const Time deadline = now_us() + 20'000'000;
  while (!stop && now_us() < deadline) {
    const Time t = now_us();
    app(c);
    while (auto p = c.poll_transmit(t)) {
      if (peer.port != 0) sock.send_to(*p, peer);
    }
    if (c.next_timeout() <= t) c.on_timeout(t);
    const Time wait = std::min<Time>(std::max<Time>(c.next_timeout() - t, 0), 5'000);
    Endpoint from;
    if (auto d = sock.receive(wait, &from)) {
      if (learn_peer && peer.port == 0) peer = from;
      c.on_datagram(*d, now_us());
    }
    if (c.finished() || c.failed()) {
      // Linger briefly so a lost final ACK can still be repeated.
      for (int i = 0; i < 20; ++i) {
        if (auto d = sock.receive(5'000, &from)) c.on_datagram(*d, now_us());
        while (auto p = c.poll_transmit(now_us())) sock.send_to(*p, peer);
      }
      return;
    }
  }
}

}  // namespace

TEST(RealSockets, TransferOverLoopback) {
  UdpSocket server_sock;
  const std::uint16_t port = server_sock.bind("127.0.0.1", 0);
  UdpSocket client_sock;
  client_sock.bind("127.0.0.1", 0);

  Config cfg;
  Connection server(Role::Server, cfg, 1);
  Connection client(Role::Client, cfg, 2);
  client.connect(now_us());

  constexpr std::size_t kBytes = 512 * 1024;
  std::vector<std::uint8_t> sent(kBytes), received;
  for (std::size_t i = 0; i < kBytes; ++i) sent[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8));

  std::atomic<bool> stop{false};
  std::size_t offset = 0;
  bool closed = false;

  std::thread t_server([&] {
    pump(server, server_sock, Endpoint{}, true,
         [&](Connection& c) {
           std::uint8_t buf[4096];
           while (std::size_t n = c.read(buf)) received.insert(received.end(), buf, buf + n);
           if (c.eof()) c.close();
         },
         stop);
  });
  pump(client, client_sock, UdpSocket::resolve("127.0.0.1", port), false,
       [&](Connection& c) {
         if (offset < kBytes) offset += c.write(std::span(sent).subspan(offset));
         else if (!closed) {
           c.close();
           closed = true;
         }
       },
       stop);
  t_server.join();

  EXPECT_TRUE(client.finished());
  EXPECT_TRUE(server.finished());
  EXPECT_EQ(received, sent);
}
