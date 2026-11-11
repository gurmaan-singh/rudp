// Send a stream across a simulated network path and report what happened.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "rudp/sim.hpp"

using namespace rudp;

static void usage() {
  std::puts(
      "rudp_sim [options]\n"
      "  --bytes N         amount of data to send            (default 4194304)\n"
      "  --rtt MS          round-trip time in milliseconds   (default 40)\n"
      "  --loss P          random loss, 0 to 1, both ways    (default 0)\n"
      "  --reorder P       chance a packet is held back      (default 0)\n"
      "  --dup P           chance a packet arrives twice     (default 0)\n"
      "  --corrupt P       chance a packet has a bit flipped (default 0)\n"
      "  --bandwidth BPS   bottleneck in bytes per second    (default unlimited)\n"
      "  --queue BYTES     bottleneck queue size             (default 65536)\n"
      "  --no-sack         use duplicate-ACK recovery only\n"
      "  --no-cc           turn congestion control off\n"
      "  --seed N          random seed                       (default 1)\n"
      "  --trace FILE      write the congestion window over time as CSV\n"
      "  --log            print every packet sent and received");
}

int main(int argc, char** argv) {
  TransferConfig c;
  c.bytes = 4 << 20;
  double rtt_ms = 40;
  double loss = 0, reorder = 0, dup = 0, corrupt = 0, bandwidth = 0;
  std::size_t queue = 64 * 1024;
  std::string trace_file;
  bool log = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--bytes") c.bytes = std::strtoull(next(), nullptr, 10);
    else if (a == "--rtt") rtt_ms = std::atof(next());
    else if (a == "--loss") loss = std::atof(next());
    else if (a == "--reorder") reorder = std::atof(next());
    else if (a == "--dup") dup = std::atof(next());
    else if (a == "--corrupt") corrupt = std::atof(next());
    else if (a == "--bandwidth") bandwidth = std::atof(next());
    else if (a == "--queue") queue = std::strtoull(next(), nullptr, 10);
    else if (a == "--seed") c.seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--no-sack") c.client.sack = c.server.sack = false;
    else if (a == "--no-cc") c.client.congestion_control = false;
    else if (a == "--trace") trace_file = next();
    else if (a == "--log") log = true;
    else {
      usage();
      return a == "--help" ? 0 : 2;
    }
  }
  for (LinkParams* l : {&c.forward, &c.reverse}) {
    l->delay = static_cast<Time>(rtt_ms * 500.0);
    l->loss = loss;
    l->bandwidth = bandwidth;
    l->queue_bytes = queue;
  }
  c.forward.reorder = reorder;
  c.forward.duplicate = dup;
  c.forward.corrupt = corrupt;
  c.trace_cwnd = !trace_file.empty();
  if (log) {
    c.log = [](Time t, char who, char what, const std::vector<std::uint8_t>& d) {
      const auto p = decode(d);
      if (!p) {
        std::printf("%10.4f %c %c  <bad packet>\n", static_cast<double>(t) / 1e6, who, what);
        return;
      }
      const Header& h = p->header;
      std::printf("%10.4f %c %c  %s%s%s%s seq=%u ack=%u win=%u len=%zu sack=%zu\n", static_cast<double>(t) / 1e6, who, what,
                  (h.flags & kSyn) ? "SYN " : "", (h.flags & kAck) ? "ACK " : "", (h.flags & kFin) ? "FIN " : "",
                  (h.flags & kProbe) ? "PROBE " : "", h.seq, h.ack, h.window, p->payload.size(), h.sack.size());
    };
  }

  const TransferResult r = run_transfer(c);
  std::printf("result        %s, data %s\n", r.completed ? "completed" : (r.failed ? "failed" : "timed out"), r.data_ok ? "intact" : "WRONG");
  std::printf("time          %.3f s (simulated)\n", static_cast<double>(r.duration) / 1e6);
  std::printf("goodput       %.2f MB/s", r.goodput / 1e6);
  if (bandwidth > 0) std::printf("  (%.0f%% of the link)", 100.0 * r.goodput / bandwidth);
  std::printf("\n");
  std::printf("sender        %llu data segments, %llu retransmitted, %llu timeouts, %llu fast recoveries\n",
              static_cast<unsigned long long>(r.client.data_segments_sent), static_cast<unsigned long long>(r.client.retransmissions),
              static_cast<unsigned long long>(r.client.timeouts), static_cast<unsigned long long>(r.client.fast_recoveries));
  std::printf("receiver      %llu out of order, %llu duplicates, %llu bad checksums\n",
              static_cast<unsigned long long>(r.server.out_of_order), static_cast<unsigned long long>(r.server.duplicate_segments),
              static_cast<unsigned long long>(r.server.bad_packets));
  std::printf("network       forward sent %llu, lost %llu, queue drops %llu, corrupted %llu\n",
              static_cast<unsigned long long>(r.forward.sent), static_cast<unsigned long long>(r.forward.dropped_loss),
              static_cast<unsigned long long>(r.forward.dropped_queue), static_cast<unsigned long long>(r.forward.corrupted));
  if (!trace_file.empty()) {
    std::ofstream f(trace_file);
    f << "time_s,cwnd,ssthresh,in_flight,srtt_ms,rto_ms\n";
    for (const auto& s : r.trace) {
      f << static_cast<double>(s.t) / 1e6 << ',' << s.cwnd << ',' << (s.ssthresh > (1u << 30) ? 0 : s.ssthresh) << ',' << s.in_flight << ','
        << static_cast<double>(s.srtt) / 1000.0 << ',' << static_cast<double>(s.rto) / 1000.0 << '\n';
    }
    std::printf("trace         %zu samples written to %s\n", r.trace.size(), trace_file.c_str());
  }
  return r.completed && r.data_ok ? 0 : 1;
}
