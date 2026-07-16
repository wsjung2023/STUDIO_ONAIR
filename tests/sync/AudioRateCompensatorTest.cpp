#include "sync/AudioRateCompensator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace {

using creator::core::ErrorCode;
using creator::synchronization::AudioRateCompensator;

TEST(AudioRateCompensatorTest, NeutralRatioNeverChangesSampleCount) {
    AudioRateCompensator compensator;

    for (int block = 0; block < 100; ++block) {
        const auto correction = compensator.next(480, 1.0);
        ASSERT_TRUE(correction.hasValue());
        EXPECT_EQ(correction.value(), 0);
    }
    EXPECT_DOUBLE_EQ(compensator.pendingSamples(), 0.0);
}

TEST(AudioRateCompensatorTest, AccumulatesSubSamplePositiveCorrection) {
    AudioRateCompensator compensator;
    int total = 0;

    for (int block = 0; block < 10; ++block) {
        const auto correction = compensator.next(480, 1.0005);
        ASSERT_TRUE(correction.hasValue());
        total += correction.value();
    }

    EXPECT_EQ(total, 2);
    EXPECT_NEAR(compensator.pendingSamples(), 0.4, 1.0e-9);
}

TEST(AudioRateCompensatorTest, AccumulatesNegativeCorrectionSymmetrically) {
    AudioRateCompensator compensator;
    int total = 0;

    for (int block = 0; block < 10; ++block) {
        const auto correction = compensator.next(480, 0.9995);
        ASSERT_TRUE(correction.hasValue());
        total += correction.value();
    }

    EXPECT_EQ(total, -2);
    EXPECT_NEAR(compensator.pendingSamples(), -0.4, 1.0e-9);
}

TEST(AudioRateCompensatorTest, RejectsUnsafeInputsWithoutMutatingAccumulator) {
    AudioRateCompensator compensator;
    ASSERT_TRUE(compensator.next(480, 1.0005).hasValue());
    const double pending = compensator.pendingSamples();

    const auto zeroFrames = compensator.next(0, 1.0);
    const auto belowClamp = compensator.next(480, 0.9989);
    const auto aboveClamp = compensator.next(480, 1.0011);
    const auto notFinite = compensator.next(
        480, std::numeric_limits<double>::quiet_NaN());

    ASSERT_FALSE(zeroFrames.hasValue());
    EXPECT_EQ(zeroFrames.error().code(), ErrorCode::InvalidArgument);
    ASSERT_FALSE(belowClamp.hasValue());
    ASSERT_FALSE(aboveClamp.hasValue());
    ASSERT_FALSE(notFinite.hasValue());
    EXPECT_DOUBLE_EQ(compensator.pendingSamples(), pending);
}

}  // namespace
