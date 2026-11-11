#include "rudp/sim.hpp"

#include <algorithm>

namespace rudp {

void SimLink::send(std::vector<std::uint8_t> packet, Time now) {
  ++stats.sent;
  Time depart = now;
  if (p_.bandwidth > 0.0) {
    const double backlog = free_at_ > now ? static_cast<double>(free_at_ - now) * p_.bandwidth / 1e6 : 0.0;
    if (backlog + static_cast<double>(packet.size()) > static_cast<double>(p_.queue_bytes)) {
      ++stats.dropped_queue;
      return;
    }
    const Time serialise = static_cast<Time>(static_cast<double>(packet.size()) * 1e6 / p_.bandwidth);
    free_at_ = std::max(free_at_, now) + serialise;
    depart = free_at_;
  }
  if (uniform() < p_.loss) {
    ++stats.dropped_loss;
    return;
  }
  const int copies = uniform() < p_.duplicate ? 2 : 1;
  if (copies == 2) ++stats.duplicated;
  for (int i = 0; i < copies; ++i) {
    std::vector<std::uint8_t> data = packet;
    if (uniform() < p_.corrupt && !data.empty()) {
      const std::size_t bit = std::uniform_int_distribution<std::size_t>(0, data.size() * 8 - 1)(rng_);
      data[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
      ++stats.corrupted;
    }
    Time at = depart + p_.delay;
    if (p_.jitter > 0) at += static_cast<Time>(uniform() * static_cast<double>(p_.jitter));
    if (uniform() < p_.reorder) at += p_.reorder_delay;
    q_.push(Item{at, counter_++, std::move(data)});
  }
}

namespace {

// Deterministic byte stream: the byte at offset i depends only on i and the seed.
std::uint8_t pattern(std::uint64_t seed, std::uint64_t i) {
  std::uint64_t x = i * 0x9E3779B97F4A7C15ULL + seed;
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  return static_cast<std::uint8_t>(x >> 56);
}

}  // namespace

TransferResult run_transfer(const TransferConfig& cfg) {
  Connection client(Role::Client, cfg.client, cfg.seed * 2 + 1);
  Connection server(Role::Server, cfg.server, cfg.seed * 2 + 2);
  SimLink fwd(cfg.forward, cfg.seed * 7 + 3);
  SimLink rev(cfg.reverse, cfg.seed * 7 + 4);
  TransferResult res;

  Time now = 0;
  client.connect(now);

  std::uint64_t produced = 0;   // bytes the client application has handed over
  std::uint64_t consumed = 0;   // bytes the server application has read
  bool data_ok = true;
  bool client_closed = false;
  bool server_closed = false;
  double read_credit = 0.0;
  Time last_read = 0;
  std::size_t last_cwnd = 0;
  std::vector<std::uint8_t> chunk(16 * 1024);

  auto to_server = [&](std::vector<std::uint8_t>&& d) {
    if (cfg.log) cfg.log(now, 'S', 'r', d);
    server.on_datagram(d, now);
  };
  auto to_client = [&](std::vector<std::uint8_t>&& d) {
    if (cfg.log) cfg.log(now, 'C', 'r', d);
    client.on_datagram(d, now);
  };

  for (std::uint64_t iter = 0;; ++iter) {
    // 1. Deliver what has arrived.
    fwd.deliver(now, to_server);
    rev.deliver(now, to_client);

    // 2. Timers.
    if (client.next_timeout() <= now) client.on_timeout(now);
    if (server.next_timeout() <= now) server.on_timeout(now);

    // 3. Applications.
    while (produced < cfg.bytes && client.writable() > 0) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>({cfg.bytes - produced, chunk.size(), client.writable()}));
      for (std::size_t i = 0; i < n; ++i) chunk[i] = pattern(cfg.seed, produced + i);
      const std::size_t took = client.write({chunk.data(), n});
      produced += took;
      if (took < n) break;
    }
    if (produced == cfg.bytes && !client_closed) {
      client.close();
      client_closed = true;
    }

    std::size_t may_read = server.readable();
    if (cfg.reader_rate > 0.0) {
      read_credit += cfg.reader_rate * static_cast<double>(now - last_read) / 1e6;
      may_read = std::min<std::size_t>(may_read, static_cast<std::size_t>(read_credit));
    }
    last_read = now;
    while (may_read > 0) {
      const std::size_t want = std::min(may_read, chunk.size());
      const std::size_t n = server.read({chunk.data(), want});
      if (n == 0) break;
      for (std::size_t i = 0; i < n; ++i) data_ok = data_ok && chunk[i] == pattern(cfg.seed, consumed + i);
      consumed += n;
      may_read -= n;
      if (cfg.reader_rate > 0.0) read_credit -= static_cast<double>(n);
    }
    if (server.eof() && !server_closed) {
      server.close();
      server_closed = true;
    }

    // 4. Transmit.
    while (auto p = client.poll_transmit(now)) {
      if (cfg.log) cfg.log(now, 'C', 's', *p);
      fwd.send(std::move(*p), now);
    }
    while (auto p = server.poll_transmit(now)) {
      if (cfg.log) cfg.log(now, 'S', 's', *p);
      rev.send(std::move(*p), now);
    }

    if (cfg.trace_cwnd && (client.cwnd() != last_cwnd || res.trace.empty() || now - res.trace.back().t > 50'000)) {
      last_cwnd = client.cwnd();
      res.trace.push_back({now, client.cwnd(), client.ssthresh(), client.bytes_in_flight(), client.srtt(), client.rto()});
    }

    if (cfg.check_invariants && res.violation.empty()) {
      std::string v = client.check_invariants();
      if (!v.empty()) res.violation = "client at " + std::to_string(now) + " us: " + v;
      else if (!(v = server.check_invariants()).empty()) res.violation = "server at " + std::to_string(now) + " us: " + v;
      if (!res.violation.empty()) break;
    }

    if (client.failed() || server.failed()) {
      res.failed = true;
      break;
    }
    if (client.finished() && server.finished()) {
      res.completed = true;
      break;
    }

    // 5. Move the clock to the next thing that can happen.
    Time next = std::min({fwd.next_delivery(), rev.next_delivery(), client.next_timeout(), server.next_timeout()});
    if (cfg.reader_rate > 0.0 && server.readable() > 0) next = std::min(next, now + 1000);
    if (next == kNever || next > cfg.time_limit) break;
    now = std::max(next, now);
    if (iter > 400'000'000ULL) break;
  }

  res.data_ok = data_ok && consumed == cfg.bytes;
  res.duration = now;
  res.goodput = now > 0 ? static_cast<double>(consumed) * 1e6 / static_cast<double>(now) : 0.0;
  res.client = client.stats();
  res.server = server.stats();
  res.forward = fwd.stats;
  res.reverse = rev.stats;
  return res;
}

}  // namespace rudp
