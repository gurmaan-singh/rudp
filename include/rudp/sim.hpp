#pragma once
#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <random>
#include <vector>

#include "rudp/connection.hpp"

namespace rudp {

// A one-way network path with the usual faults, driven by a virtual clock so runs are exact and repeatable.
struct LinkParams {
  Time delay = 20'000;            // one-way propagation delay, microseconds
  Time jitter = 0;                // extra delay, uniform in [0, jitter]; can reorder packets
  double loss = 0.0;              // probability a packet is dropped at random
  double duplicate = 0.0;         // probability a packet arrives twice
  double reorder = 0.0;           // probability a packet is held back by `reorder_delay`
  Time reorder_delay = 30'000;
  double corrupt = 0.0;           // probability one bit is flipped
  double bandwidth = 0.0;         // bytes per second through the bottleneck; 0 means unlimited
  std::size_t queue_bytes = 64 * 1024;  // bottleneck buffer: a drop-tail queue, used only when bandwidth is set
};

struct LinkStats {
  std::uint64_t sent = 0;
  std::uint64_t delivered = 0;
  std::uint64_t dropped_loss = 0;
  std::uint64_t dropped_queue = 0;
  std::uint64_t duplicated = 0;
  std::uint64_t corrupted = 0;
};

class SimLink {
 public:
  SimLink(const LinkParams& p, std::uint64_t seed) : p_(p), rng_(seed) {}

  void send(std::vector<std::uint8_t> packet, Time now);
  // Hand every packet that has arrived by `now` to `fn(std::vector<uint8_t>&&)`.
  template <class F>
  void deliver(Time now, F&& fn) {
    while (!q_.empty() && q_.top().at <= now) {
      Item it = q_.top();
      q_.pop();
      ++stats.delivered;
      fn(std::move(it.data));
    }
  }
  Time next_delivery() const { return q_.empty() ? kNever : q_.top().at; }
  LinkStats stats;

 private:
  struct Item {
    Time at;
    std::uint64_t order;
    std::vector<std::uint8_t> data;
    bool operator<(const Item& o) const { return at != o.at ? at > o.at : order > o.order; }
  };
  double uniform() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }

  LinkParams p_;
  std::mt19937_64 rng_;
  std::priority_queue<Item> q_;
  std::uint64_t counter_ = 0;
  Time free_at_ = 0;  // when the bottleneck finishes sending what it already has
};

struct CwndSample {
  Time t;
  std::size_t cwnd;
  std::size_t ssthresh;
  std::size_t in_flight;
  Time srtt;
  Time rto;
};

struct TransferConfig {
  std::size_t bytes = 1 << 20;
  Config client;
  Config server;
  LinkParams forward;   // client to server
  LinkParams reverse;   // server to client
  std::uint64_t seed = 1;
  Time time_limit = 600'000'000;
  double reader_rate = 0.0;      // bytes per second the server application reads at; 0 means it reads at once
  bool trace_cwnd = false;
  bool check_invariants = false;  // verify both connections' internal state after every step
  // Optional packet log: called with the time, 'C' or 'S' for the sender, 's' (sent) or 'r' (received), and the datagram.
  std::function<void(Time, char, char, const std::vector<std::uint8_t>&)> log;
};

struct TransferResult {
  bool completed = false;        // both ends finished cleanly
  bool data_ok = false;          // every byte arrived, in order, unchanged
  bool failed = false;           // a connection gave up
  Time duration = 0;             // virtual microseconds until both ends were finished
  double goodput = 0.0;          // application bytes per second
  Stats client;
  Stats server;
  LinkStats forward;
  LinkStats reverse;
  std::vector<CwndSample> trace;
  std::string violation;         // first internal inconsistency found, if checking was on
};

// Send `bytes` of pseudo-random data from a client to a server over the simulated path and check it arrives intact.
TransferResult run_transfer(const TransferConfig& cfg);

}  // namespace rudp
