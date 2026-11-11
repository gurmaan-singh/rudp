#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "rudp/packet.hpp"
#include "rudp/rtt.hpp"
#include "rudp/seq.hpp"

namespace rudp {

constexpr Time kNever = std::numeric_limits<Time>::max();

enum class Role { Client, Server };

struct Config {
  std::size_t mss = 1200;                 // payload bytes per packet
  std::size_t send_buffer = 1 << 20;      // bytes the application may have queued
  std::size_t recv_buffer = 1 << 20;      // bytes held for the application, which is also the advertised window
  Time initial_rto = 1'000'000;
  Time min_rto = 200'000;
  Time max_rto = 60'000'000;
  std::size_t initial_cwnd_segments = 10;
  int max_retries = 12;                   // consecutive timeouts before the connection is declared dead
  bool sack = true;                       // selective acknowledgements; off falls back to duplicate-ACK recovery
  bool congestion_control = true;         // off: send as fast as the peer's window allows
  std::optional<std::uint32_t> initial_seq;  // fixed first sequence number, for testing wraparound
};

struct Stats {
  std::uint64_t packets_sent = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t bad_packets = 0;          // too short, wrong magic or failed checksum
  std::uint64_t data_segments_sent = 0;
  std::uint64_t retransmissions = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t fast_recoveries = 0;      // times a loss was detected without waiting for the timer
  std::uint64_t dup_acks = 0;
  std::uint64_t out_of_order = 0;         // data segments that arrived ahead of a gap
  std::uint64_t duplicate_segments = 0;   // data that had already been received
  std::uint64_t window_drops = 0;         // data discarded because the receive buffer was full
  std::uint64_t bytes_acked = 0;
  std::uint64_t bytes_delivered = 0;      // handed to the application by read()
};

// One end of a reliable, ordered byte stream carried in datagrams.
//
// The class does no I/O and never looks at a clock. The caller passes in the current time
// and moves datagrams in and out, which makes it usable over real sockets and inside a
// deterministic simulation alike:
//
//     conn.on_datagram(bytes, now);              // for every datagram that arrives
//     while (auto d = conn.poll_transmit(now)) send(*d);
//     ...wait until conn.next_timeout() or the next datagram...
//     conn.on_timeout(now);                      // when now >= next_timeout()
class Connection {
 public:
  Connection(Role role, const Config& config, std::uint64_t seed);

  void connect(Time now);  // client only: start the handshake
  void on_datagram(std::span<const std::uint8_t> datagram, Time now);
  std::optional<std::vector<std::uint8_t>> poll_transmit(Time now);
  Time next_timeout() const;
  void on_timeout(Time now);

  // Queue bytes to send. Returns how many were accepted (fewer than offered if the buffer is full).
  std::size_t write(std::span<const std::uint8_t> data);
  // Take received bytes. Returns 0 if none are ready; check eof() to tell "nothing yet" from "finished".
  std::size_t read(std::span<std::uint8_t> out);
  // No more data will be written. The FIN goes out once the queued data has.
  void close();

  bool established() const { return phase_ == Phase::Open; }
  bool failed() const { return phase_ == Phase::Failed; }
  bool eof() const { return peer_fin_ && recv_buf_.empty(); }
  bool finished() const { return phase_ == Phase::Open && fin_acked_ && peer_fin_; }
  std::size_t writable() const { return cfg_.send_buffer - send_q_.size(); }
  std::size_t readable() const { return recv_buf_.size(); }

  std::size_t cwnd() const { return cwnd_; }
  std::size_t ssthresh() const { return ssthresh_; }
  std::size_t bytes_in_flight() const { return static_cast<std::size_t>(pipe_); }
  Time srtt() const { return rtt_.srtt(); }
  Time rto() const { return rto_; }
  const Stats& stats() const { return stats_; }

  // Internal consistency checks for tests and debugging: returns an empty string if all is well,
  // otherwise a description of the first thing found wrong.
  std::string check_invariants() const;

 private:
  enum class Phase { Idle, SynSent, SynRcvd, Open, Failed };
  enum class SegState : std::uint8_t { Outstanding, Sacked, Lost };

  struct Segment {
    std::uint32_t seq = 0;
    std::uint32_t len = 0;       // sequence numbers used: payload bytes, or 1 for SYN and FIN
    std::uint8_t flags = 0;      // kSyn or kFin for control segments, 0 for data
    std::vector<std::uint8_t> data;
    Time sent = 0;
    SegState state = SegState::Lost;  // a new segment starts as "needs sending"
    bool sent_once = false;
    bool retx = false;           // sent more than once, so its RTT would be ambiguous
  };

  void process_ack(const Header& h, bool pure, Time now);
  void process_data(const Header& h, std::span<const std::uint8_t> payload);
  void drain_out_of_order();
  void mark_sacked(const std::vector<SackBlock>& blocks, std::size_t& newly);
  void detect_loss(Time now);
  void enter_recovery();
  void mark_lost(Segment& s);
  void set_outstanding(Segment& s);
  void cc_on_ack(std::size_t newly);
  bool can_send(std::size_t len) const;
  Segment* pick_segment(Time now);
  std::vector<std::uint8_t> build(std::uint8_t flags, std::uint32_t seq, std::span<const std::uint8_t> payload);
  std::size_t free_window() const { return cfg_.recv_buffer - recv_buf_.size(); }

  Role role_;
  Config cfg_;
  std::mt19937_64 rng_;
  Phase phase_ = Phase::Idle;
  Stats stats_;

  // sender
  std::uint32_t iss_ = 0;
  std::uint32_t snd_una_ = 0;
  std::uint32_t snd_nxt_ = 0;
  std::deque<std::uint8_t> send_q_;
  std::deque<Segment> inflight_;
  std::uint32_t rwnd_ = 0;
  std::uint32_t pipe_ = 0;         // bytes believed to be in the network: outstanding and not lost
  std::size_t lost_count_ = 0;
  std::size_t cwnd_ = 0;
  std::size_t ssthresh_ = std::numeric_limits<std::uint32_t>::max();
  bool in_recovery_ = false;
  std::uint32_t recover_ = 0;
  int dupacks_ = 0;
  int consecutive_timeouts_ = 0;
  RttEstimator rtt_;
  Time rto_;
  Time rto_deadline_ = kNever;
  Time persist_deadline_ = kNever;
  Time persist_interval_ = 0;
  bool probe_pending_ = false;
  bool fin_queued_ = false;
  bool fin_sent_ = false;
  bool fin_acked_ = false;

  // receiver
  std::uint32_t irs_ = 0;
  std::uint32_t rcv_nxt_ = 0;
  std::deque<std::uint8_t> recv_buf_;
  std::map<std::uint32_t, std::vector<std::uint8_t>, SeqLess> ooo_;
  std::optional<std::uint32_t> fin_seq_;
  bool peer_fin_ = false;
  bool ack_pending_ = false;
  bool resend_synack_ = false;
  std::size_t last_advertised_ = 0;
  std::uint32_t last_ts_rx_ = 0;   // timestamp of the latest packet received, echoed back
  Time clock_ = 0;                 // the latest time the caller has told us
  std::uint32_t ts_now() const { return static_cast<std::uint32_t>(clock_) + 1U; }
};

}  // namespace rudp
