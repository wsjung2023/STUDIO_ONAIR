#include "core/Timebase.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <type_traits>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::frameToTimestamp;
using creator::core::ProjectClock;
using creator::core::TimestampNs;
using creator::core::timestampToFrame;

// A timestamp and a duration must not be the same type. If they are, adding two
// timestamps compiles, and the "단위 없는 정수 시간 금지" rule in CLAUDE.md 4
// buys us nothing.
static_assert(!std::is_same_v<TimestampNs, DurationNs>,
              "TimestampNs and DurationNs must be distinct types");
static_assert(std::is_same_v<decltype(std::declval<TimestampNs>() - std::declval<TimestampNs>()),
                             DurationNs>,
              "timestamp - timestamp must yield a duration");
static_assert(std::is_same_v<decltype(std::declval<TimestampNs>() + std::declval<DurationNs>()),
                             TimestampNs>,
              "timestamp + duration must yield a timestamp");

TEST(TimebaseTest, ClockIsSteady) {
    // Wall clock must never drive A/V sync (ARCHITECTURE.md 5.1, CLAUDE.md 9).
    //
    // EXPECT_TRUE(ProjectClock::is_steady) alone cannot catch a regression:
    // is_steady is just a `static constexpr bool is_steady = true;` read back
    // from the header, so a swap to system_clock that carried that same
    // (now-lying) constant along would still pass it. ClockAdvancesMonotonically
    // below cannot catch it either - system_clock also advances between two
    // reads. What actually distinguishes the two clocks is their epoch:
    // system_clock's is 1970 (~1.8e18ns today), steady_clock's is boot or
    // some other arbitrary point (~1e13-1e16ns on a real machine, orders of
    // magnitude smaller). Comparing ProjectClock::now() against
    // steady_clock::now() directly makes a system_clock swap impossible to
    // miss: the two would differ by decades' worth of nanoseconds instead of
    // by a few instructions' worth.
    EXPECT_TRUE(ProjectClock::is_steady);
    const auto drift = (ProjectClock::now().time_since_epoch() -
                         std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    EXPECT_LT(std::abs(drift), 1'000'000'000LL);
}

TEST(TimebaseTest, ClockAdvancesMonotonically) {
    const TimestampNs first = ProjectClock::now();
    const TimestampNs second = ProjectClock::now();

    EXPECT_LE(first, second);
}

TEST(TimebaseTest, ConvertsBetweenUnits) {
    const DurationNs oneSecond = std::chrono::seconds{1};
    EXPECT_EQ(oneSecond.count(), 1'000'000'000);

    const DurationNs oneMilli = std::chrono::milliseconds{1};
    EXPECT_EQ(oneMilli.count(), 1'000'000);

    const auto asMillis = std::chrono::duration_cast<std::chrono::milliseconds>(oneSecond);
    EXPECT_EQ(asMillis.count(), 1000);
}

TEST(FrameRateTest, AcceptsIntegerRate) {
    const auto rate = FrameRate::create(60, 1);

    ASSERT_TRUE(rate.hasValue());
    EXPECT_EQ(rate.value().numerator(), 60);
    EXPECT_EQ(rate.value().denominator(), 1);
}

TEST(FrameRateTest, AcceptsDropFrameRate) {
    // 59.94 is exactly 60000/1001. Storing it as a double and rounding drifts
    // over a two hour recording, which is what ARCHITECTURE.md 5.3 budgets at
    // 40ms total.
    const auto rate = FrameRate::create(60000, 1001);

    ASSERT_TRUE(rate.hasValue());
    EXPECT_EQ(rate.value().numerator(), 60000);
    EXPECT_EQ(rate.value().denominator(), 1001);
}

TEST(FrameRateTest, RejectsZeroDenominator) {
    const auto rate = FrameRate::create(60, 0);

    ASSERT_FALSE(rate.hasValue());
    EXPECT_EQ(rate.error().code(), ErrorCode::InvalidArgument);
}

TEST(FrameRateTest, RejectsZeroNumerator) {
    const auto rate = FrameRate::create(0, 1);

    ASSERT_FALSE(rate.hasValue());
    EXPECT_EQ(rate.error().code(), ErrorCode::InvalidArgument);
}

TEST(FrameRateTest, RejectsNegativeRate) {
    EXPECT_FALSE(FrameRate::create(-60, 1).hasValue());
    EXPECT_FALSE(FrameRate::create(60, -1).hasValue());
}

TEST(FrameConversionTest, FrameZeroIsTimeZero) {
    const auto rate = FrameRate::create(60, 1);
    ASSERT_TRUE(rate.hasValue());

    EXPECT_EQ(frameToTimestamp(0, rate.value()).time_since_epoch().count(), 0);
}

TEST(FrameConversionTest, ConvertsIntegerRate) {
    const auto rate = FrameRate::create(60, 1);
    ASSERT_TRUE(rate.hasValue());

    // Frame 60 at 60fps is exactly one second in.
    EXPECT_EQ(frameToTimestamp(60, rate.value()).time_since_epoch().count(), 1'000'000'000);
}

TEST(FrameConversionTest, RoundTripsAtIntegerRate) {
    const auto rate = FrameRate::create(60, 1);
    ASSERT_TRUE(rate.hasValue());

    for (std::int64_t frame : {0LL, 1LL, 59LL, 60LL, 3600LL, 432000LL}) {
        const TimestampNs timestamp = frameToTimestamp(frame, rate.value());
        EXPECT_EQ(timestampToFrame(timestamp, rate.value()), frame) << "frame " << frame;
    }
}

TEST(FrameConversionTest, RoundTripsAtDropFrameRate) {
    const auto rate = FrameRate::create(60000, 1001);
    ASSERT_TRUE(rate.hasValue());

    // 432000 frames is two hours at 60fps: the soak duration ARCHITECTURE.md 5.3
    // sets targets for, so the arithmetic must stay exact that far out.
    for (std::int64_t frame : {0LL, 1LL, 1000LL, 432000LL}) {
        const TimestampNs timestamp = frameToTimestamp(frame, rate.value());
        EXPECT_EQ(timestampToFrame(timestamp, rate.value()), frame) << "frame " << frame;
    }
}

TEST(FrameConversionTest, TwoHoursAtDropFrameRateDoesNotDrift) {
    const auto rate = FrameRate::create(60000, 1001);
    ASSERT_TRUE(rate.hasValue());

    // 2h of 59.94fps = 431568 frames = 431568 * 1001/60000 s = 7199.9928s
    // (431568 = floor(7200 * 60000/1001): 2 hours does not divide evenly into
    // 59.94fps frames, so 431568 whole frames fall 7.2ms short of 7200s).
    const TimestampNs timestamp = frameToTimestamp(431568, rate.value());
    EXPECT_EQ(timestamp.time_since_epoch().count(), 7'199'992'800'000LL);
}

}  // namespace
