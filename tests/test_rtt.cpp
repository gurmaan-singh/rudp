#include <gtest/gtest.h>

#include "rudp/rtt.hpp"

using namespace rudp;

TEST(Rtt, FirstSampleFollowsRfc6298) {
  RttEstimator r(1'000'000, 1, 60'000'000);
  EXPECT_EQ(r.rto(), 1'000'000);  // before any sample
  r.sample(100'000);
  EXPECT_EQ(r.srtt(), 100'000);
  EXPECT_EQ(r.rttvar(), 50'000);
  EXPECT_EQ(r.rto(), 100'000 + 4 * 50'000);
}

TEST(Rtt, LaterSamplesUseTheSmoothingWeights) {
  RttEstimator r(1'000'000, 1, 60'000'000);
  r.sample(100'000);
  r.sample(200'000);
  // rttvar = 3/4 * 50000 + 1/4 * |100000 - 200000| = 62500; srtt = 7/8 * 100000 + 1/8 * 200000 = 112500
  EXPECT_EQ(r.rttvar(), 62'500);
  EXPECT_EQ(r.srtt(), 112'500);
  EXPECT_EQ(r.rto(), 112'500 + 4 * 62'500);
}

TEST(Rtt, SteadySamplesConvergeAndShrinkTheVariance) {
  RttEstimator r(1'000'000, 1, 60'000'000);
  for (int i = 0; i < 200; ++i) r.sample(50'000);
  EXPECT_NEAR(static_cast<double>(r.srtt()), 50'000.0, 1.0);
  EXPECT_LT(r.rttvar(), 100);
}

TEST(Rtt, TimeoutIsClamped) {
  RttEstimator r(1'000'000, 200'000, 2'000'000);
  r.sample(1'000);  // a very fast link would give a tiny RTO; the floor keeps it sane
  EXPECT_EQ(r.rto(), 200'000);
  r.sample(5'000'000);
  EXPECT_EQ(r.rto(), 2'000'000);
}
