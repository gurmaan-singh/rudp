// Goodput against loss rate, for a few configurations. Writes CSV on stdout.
//   rudp_bench > results.csv
#include <cmath>
#include <cstdio>

#include "rudp/sim.hpp"

using namespace rudp;

namespace {

struct Variant {
  const char* name;
  bool sack;
  bool cc;
};

double mean_goodput(const Variant& v, double loss, double rtt_ms, int seeds, std::size_t bytes) {
  double sum = 0;
  for (int s = 1; s <= seeds; ++s) {
    TransferConfig c;
    c.bytes = bytes;
    c.seed = static_cast<std::uint64_t>(s);
    c.client.sack = c.server.sack = v.sack;
    c.client.congestion_control = v.cc;
    for (LinkParams* l : {&c.forward, &c.reverse}) {
      l->delay = static_cast<Time>(rtt_ms * 500.0);
      l->loss = loss;
    }
    c.time_limit = 3'600'000'000LL;
    const auto r = run_transfer(c);
    if (!r.completed || !r.data_ok) std::fprintf(stderr, "warning: %s loss %.3f seed %d did not complete\n", v.name, loss, s);
    sum += r.goodput;
  }
  return sum / seeds;
}

}  // namespace

int main() {
  const Variant variants[] = {{"sack+cc", true, true}, {"dupack+cc", false, true}, {"sack-no-cc", true, false}};
  const double losses[] = {0.0, 0.001, 0.003, 0.01, 0.02, 0.05, 0.10, 0.15, 0.20};
  const double rtts[] = {20, 80, 200};
  std::puts("rtt_ms,loss,variant,goodput_MBps,mathis_MBps");
  for (double rtt : rtts) {
    for (double loss : losses) {
      // Mathis et al.: throughput is about 1.22 * MSS / (RTT * sqrt(loss)) for a Reno-like sender.
      const double mathis = loss > 0 ? 1.22 * 1200.0 / (rtt / 1000.0 * std::sqrt(loss)) / 1e6 : 0.0;
      for (const Variant& v : variants) {
        const std::size_t bytes = loss >= 0.05 ? 1 << 20 : 4 << 20;
        std::printf("%.0f,%.3f,%s,%.4f,%.4f\n", rtt, loss, v.name, mean_goodput(v, loss, rtt, 5, bytes) / 1e6, mathis);
        std::fflush(stdout);
      }
    }
  }
  return 0;
}
