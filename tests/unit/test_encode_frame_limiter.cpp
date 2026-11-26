/**
 * @file tests/unit/test_encode_frame_limiter.cpp
 * @brief Test the encoder loop's frame-rate limiter (video::encode_frame_limiter_t).
 */
#include "../tests_common.h"

#include <src/video.h>

using namespace std::literals;
using clock_type = std::chrono::steady_clock;

namespace {
  constexpr auto interval = std::chrono::nanoseconds {1s} / 120;  // 8.33 ms
  const auto t0 = clock_type::time_point {} + 10s;
}  // namespace

// Bugs caught: 1, False positives: 0
// The ext capture backend re-sends the unchanged frame every 100 ms. A real frame that followed such a repeat
// within 6.25 ms was dropped, so the change reached the client only with the next repeat, ~100 ms late
// (seen in end-to-end latency runs on 2026-11-25).
TEST(EncodeFrameLimiterTest, RealFrameRightAfterARepeatIsEncoded) {
  video::encode_frame_limiter_t limiter {interval};
  ASSERT_TRUE(limiter.admit(t0, false));
  ASSERT_TRUE(limiter.admit(t0 + 100ms, true));  // idle repeat

  auto admitted = limiter.admit(t0 + 102ms, false);
  ASSERT_TRUE(admitted);
  EXPECT_EQ(*admitted, t0 + 102ms);
}

// Bugs caught: 0, False positives: 0
TEST(EncodeFrameLimiterTest, RealFramesFasterThanTheFrameRateAreDropped) {
  video::encode_frame_limiter_t limiter {interval};
  ASSERT_TRUE(limiter.admit(t0, false));
  EXPECT_FALSE(limiter.admit(t0 + 3ms, false));
  EXPECT_TRUE(limiter.admit(t0 + interval, false));
}

// Bugs caught: 0, False positives: 0
TEST(EncodeFrameLimiterTest, SlightlyEarlyFrameIsStampedWithItsSlot) {
  video::encode_frame_limiter_t limiter {interval};
  ASSERT_TRUE(limiter.admit(t0, false));
  auto admitted = limiter.admit(t0 + interval - 1ms, false);
  ASSERT_TRUE(admitted);
  EXPECT_EQ(*admitted, t0 + interval);
}
