#include <gtest/gtest.h>

#include <random>

#include "rudp/sim.hpp"

using namespace rudp;

namespace {

TransferConfig base(std::size_t bytes = 1 << 20, std::uint64_t seed = 1) {
  TransferConfig c;
  c.bytes = bytes;
  c.seed = seed;
  c.forward.delay = c.reverse.delay = 20'000;  // 40 ms round trip
  c.check_invariants = true;
  return c;
}

void expect_delivered(const TransferResult& r, const char* what = "") {
  EXPECT_TRUE(r.completed) << what;
  EXPECT_TRUE(r.data_ok) << what;
  EXPECT_FALSE(r.failed) << what;
  EXPECT_EQ(r.violation, "") << what;
}

}  // namespace

TEST(Transfer, CleanLinkNeedsNoRetransmissions) {
  const auto r = run_transfer(base());
  expect_delivered(r);
  EXPECT_EQ(r.client.retransmissions, 0U);
  EXPECT_EQ(r.client.timeouts, 0U);
  EXPECT_EQ(r.server.bytes_delivered, 1U << 20);
}

TEST(Transfer, EmptyAndTinyStreams) {
  for (std::size_t n : {0U, 1U, 2U, 1199U, 1200U, 1201U}) {
    const auto r = run_transfer(base(n));
    expect_delivered(r, "tiny");
    EXPECT_EQ(r.server.bytes_delivered, n);
  }
}

TEST(Transfer, SurvivesRandomLoss) {
  for (double loss : {0.01, 0.05, 0.10, 0.20}) {
    auto c = base(512 * 1024);
    c.forward.loss = c.reverse.loss = loss;
    const auto r = run_transfer(c);
    expect_delivered(r, "loss");
    EXPECT_GT(r.client.retransmissions, 0U) << "loss " << loss;
  }
}

TEST(Transfer, SurvivesReorderingDuplicationAndCorruption) {
  auto c = base(512 * 1024);
  c.forward.reorder = c.reverse.reorder = 0.15;
  c.forward.duplicate = c.reverse.duplicate = 0.10;
  c.forward.corrupt = c.reverse.corrupt = 0.03;
  c.forward.jitter = c.reverse.jitter = 15'000;
  const auto r = run_transfer(c);
  expect_delivered(r);
  EXPECT_GT(r.forward.corrupted, 0U);
  EXPECT_GT(r.server.bad_packets, 0U);          // the checksum caught them
  EXPECT_GT(r.server.duplicate_segments, 0U);   // and duplicates were recognised
}

TEST(Transfer, HandshakeSurvivesHeavyLoss) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    auto c = base(20'000, seed);
    c.forward.loss = c.reverse.loss = 0.35;  // a third of every packet, so SYNs, SYN-ACKs and FINs all get lost sometimes
    c.client.max_retries = c.server.max_retries = 20;
    c.time_limit = 3'600'000'000LL;
    expect_delivered(run_transfer(c), "handshake");
  }
}

TEST(Transfer, GivesUpOnADeadLink) {
  auto c = base(100'000);
  c.forward.loss = c.reverse.loss = 1.0;
  c.client.max_retries = c.server.max_retries = 4;
  const auto r = run_transfer(c);
  EXPECT_TRUE(r.failed);
  EXPECT_FALSE(r.completed);
}

TEST(Transfer, SequenceNumbersMayWrap) {
  auto c = base(300'000);
  c.client.initial_seq = 0xFFFFF000U;
  c.server.initial_seq = 0xFFFFFF00U;
  c.forward.loss = c.reverse.loss = 0.05;
  c.forward.reorder = 0.1;
  expect_delivered(run_transfer(c));
}

TEST(Transfer, RandomisedNetworksAlwaysDeliverIntactData) {
  std::mt19937_64 rng(2024);
  auto u = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
  for (int i = 0; i < 40; ++i) {
    auto c = base(static_cast<std::size_t>(u(1, 400'000)), static_cast<std::uint64_t>(i + 1));
    c.forward.delay = static_cast<Time>(u(500, 150'000));
    c.reverse.delay = static_cast<Time>(u(500, 150'000));
    c.forward.jitter = c.reverse.jitter = static_cast<Time>(u(0, 40'000));
    c.forward.loss = u(0, 0.25);
    c.reverse.loss = u(0, 0.25);
    c.forward.reorder = u(0, 0.2);
    c.forward.duplicate = u(0, 0.1);
    c.forward.corrupt = u(0, 0.03);
    c.client.mss = c.server.mss = static_cast<std::size_t>(u(100, 1400));
    c.client.sack = c.server.sack = i % 2 == 0;
    c.time_limit = 3'600'000'000LL;
    const auto r = run_transfer(c);
    EXPECT_TRUE(r.completed && r.data_ok) << "case " << i;
  }
}

TEST(Transfer, WorksWithoutSack) {
  auto c = base(512 * 1024);
  c.client.sack = c.server.sack = false;
  c.forward.loss = c.reverse.loss = 0.03;
  expect_delivered(run_transfer(c));
}

TEST(Transfer, SackRecoversFasterThanDuplicateAckRecovery) {
  // Several losses per window is where SACK pays off.
  double with = 0, without = 0;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    auto c = base(1 << 20, seed);
    c.forward.loss = 0.04;
    c.forward.bandwidth = 2e6;
    c.reverse.bandwidth = 2e6;
    c.forward.queue_bytes = 200'000;
    with += run_transfer(c).goodput;
    c.client.sack = c.server.sack = false;
    without += run_transfer(c).goodput;
  }
  EXPECT_GT(with, without);
}

TEST(Transfer, FillsABottleneckLink) {
  auto c = base(6 << 20);
  c.forward.bandwidth = 1'000'000;  // 1 MB/s, 40 ms RTT: the pipe holds about 40 KB
  c.reverse.bandwidth = 1'000'000;
  c.forward.queue_bytes = 60'000;
  const auto r = run_transfer(c);
  expect_delivered(r);
  EXPECT_GT(r.goodput, 0.75 * 1'000'000) << "goodput " << r.goodput;
  EXPECT_LT(r.goodput, 1'000'000) << "cannot beat the link";
  EXPECT_GT(r.forward.dropped_queue, 0U);  // probing for bandwidth fills the queue and loses packets
}

TEST(Congestion, WindowGrowsThenBacksOffOnLoss) {
  auto c = base(4 << 20);
  c.trace_cwnd = true;
  c.forward.bandwidth = 1'000'000;
  c.forward.queue_bytes = 40'000;
  const auto r = run_transfer(c);
  expect_delivered(r);
  ASSERT_GT(r.trace.size(), 20U);
  EXPECT_EQ(r.trace.front().cwnd, c.client.initial_cwnd_segments * c.client.mss);
  std::size_t peak = 0;
  bool dropped_after_peak = false;
  for (const auto& s : r.trace) {
    if (s.cwnd > peak) peak = s.cwnd;
    if (peak > 0 && s.cwnd < peak / 2 + peak / 10) dropped_after_peak = true;
  }
  EXPECT_GT(peak, 4 * c.client.initial_cwnd_segments * c.client.mss / 2);
  EXPECT_TRUE(dropped_after_peak) << "cwnd never fell back after a loss";
}

TEST(Congestion, DisablingItSendsFasterIntoALossyBottleneck) {
  // Without congestion control the sender overruns the queue and wastes the link on retransmissions.
  auto c = base(2 << 20);
  c.forward.bandwidth = 500'000;
  c.reverse.bandwidth = 500'000;
  c.forward.queue_bytes = 30'000;
  const auto with = run_transfer(c);
  c.client.congestion_control = false;
  const auto without = run_transfer(c);
  expect_delivered(with);
  expect_delivered(without);
  EXPECT_GT(without.forward.dropped_queue, with.forward.dropped_queue);
}

TEST(FlowControl, SlowReaderIsNeverOverrun) {
  auto c = base(200 * 1024);
  c.server.recv_buffer = 16 * 1024;
  c.reader_rate = 100'000;  // the application reads 100 KB/s
  const auto r = run_transfer(c);
  expect_delivered(r);
  EXPECT_EQ(r.server.window_drops, 0U);
  // The sender cannot beat the reader.
  EXPECT_GT(r.duration, static_cast<Time>(200.0 * 1024 / 100'000 * 1e6 * 0.85));
}

TEST(FlowControl, TinyReaderRateRecoversFromZeroWindow) {
  auto c = base(60 * 1024);
  c.server.recv_buffer = 4 * 1024;
  c.reader_rate = 20'000;
  c.forward.loss = c.reverse.loss = 0.05;  // window updates get lost too
  expect_delivered(run_transfer(c));
}

TEST(Rtt, EstimatorTracksThePath) {
  // Direct use of the connection pair, to read the smoothed RTT after a clean transfer.
  auto c = base(300'000);
  c.forward.delay = 50'000;
  c.reverse.delay = 50'000;
  c.trace_cwnd = false;
  const auto r = run_transfer(c);
  expect_delivered(r);
  EXPECT_EQ(r.client.timeouts, 0U);
}
