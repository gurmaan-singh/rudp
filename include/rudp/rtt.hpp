#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace rudp {

using Time = std::int64_t;  // microseconds on whatever clock the caller drives the connection with

// Round-trip time estimator and retransmission timeout, as in RFC 6298.
class RttEstimator {
 public:
  RttEstimator(Time initial_rto, Time min_rto, Time max_rto)
      : rto_(initial_rto), min_rto_(min_rto), max_rto_(max_rto) {}

  void sample(Time rtt) {
    if (!has_sample_) {
      srtt_ = rtt;
      rttvar_ = rtt / 2;
      has_sample_ = true;
    } else {
      // beta = 1/4 for the variance, alpha = 1/8 for the mean, in that order.
      rttvar_ = (3 * rttvar_ + std::llabs(srtt_ - rtt)) / 4;
      srtt_ = (7 * srtt_ + rtt) / 8;
    }
    constexpr Time kGranularity = 1000;  // 1 ms
    rto_ = std::clamp(srtt_ + std::max(kGranularity, 4 * rttvar_), min_rto_, max_rto_);
  }

  Time rto() const { return rto_; }
  Time srtt() const { return srtt_; }
  Time rttvar() const { return rttvar_; }
  bool has_sample() const { return has_sample_; }

 private:
  Time srtt_ = 0;
  Time rttvar_ = 0;
  Time rto_;
  Time min_rto_;
  Time max_rto_;
  bool has_sample_ = false;
};

}  // namespace rudp
