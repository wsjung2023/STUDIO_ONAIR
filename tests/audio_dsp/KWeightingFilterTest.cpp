#include "audio_dsp/KWeightingFilter.h"

#include "audio_dsp/AudioFormat.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cmath>

namespace creator::audio_dsp {
namespace {

AudioFormat stereo48k() { return AudioFormat::create(48'000, 2).value(); }

// The cascade's very first impulse-response sample is the product of the two
// stages' b0 coefficients (stage 2 b0 = 1.0), i.e. stage 1 b0. This pins the
// coefficient normalisation (each stage divided by its a0) to the spec sheet.
TEST(KWeightingFilterTest, ImpulseResponseFirstSampleEqualsCascadeB0) {
    auto filter = KWeightingFilter::create(stereo48k());
    ASSERT_TRUE(filter.hasValue());

    const float first = filter.value().processSample(0, 1.0F);
    EXPECT_NEAR(first, 1.53512485958697F, 1e-5F);
}

// A high-pass (stage 2 is an RLB high-pass) must reject DC: a constant input
// settles toward zero output.
TEST(KWeightingFilterTest, DcIsAttenuatedToNearZero) {
    auto filter = KWeightingFilter::create(stereo48k());
    ASSERT_TRUE(filter.hasValue());

    float last = 0.0F;
    for (int i = 0; i < 48'000; ++i) {  // 1 s of DC
        last = filter.value().processSample(0, 1.0F);
    }
    EXPECT_LT(std::abs(last), 1e-3F);
}

// Bounded input must produce bounded (finite, non-exploding) output — the
// cascade is a stable filter.
TEST(KWeightingFilterTest, BoundedInputStaysBounded) {
    auto filter = KWeightingFilter::create(stereo48k());
    ASSERT_TRUE(filter.hasValue());

    float maxAbs = 0.0F;
    for (int i = 0; i < 96'000; ++i) {
        // Deterministic bounded excitation in [-1, 1].
        const float x = std::sin(0.01F * static_cast<float>(i)) *
                        std::cos(0.003F * static_cast<float>(i));
        const float y = filter.value().processSample(0, x);
        ASSERT_TRUE(std::isfinite(y));
        maxAbs = std::max(maxAbs, std::abs(y));
    }
    // K-weighting adds at most a few dB of HF shelf gain; output must not blow up.
    EXPECT_LT(maxAbs, 4.0F);
}

// Independent per-channel state: filtering channel 0 must not disturb channel 1.
TEST(KWeightingFilterTest, ChannelStatesAreIndependent) {
    auto filterA = KWeightingFilter::create(stereo48k());
    auto filterB = KWeightingFilter::create(stereo48k());
    ASSERT_TRUE(filterA.hasValue());
    ASSERT_TRUE(filterB.hasValue());

    // Drive channel 0 of filterA with noise-ish input, leave channel 1 idle.
    float sink = 0.0F;
    for (int i = 0; i < 1000; ++i) {
        sink += filterA.value().processSample(
            0, std::sin(0.05F * static_cast<float>(i)));
    }
    ASSERT_TRUE(std::isfinite(sink));
    // filterB channel 1 gets the impulse; filterA channel 1 has never been used.
    const float aCh1 = filterA.value().processSample(1, 1.0F);
    const float bCh1 = filterB.value().processSample(1, 1.0F);
    EXPECT_FLOAT_EQ(aCh1, bCh1);
}

// Denormal flush: the RLB stage has a very slow pole (a2 ~ 0.99007), so its
// feedback state decays for thousands of samples after silence. Without a flush
// that state lingers as a subnormal and stalls the x86 FPU (CLAUDE.md §9). After
// a loud excitation followed by a long block of exact silence, the persisted
// state must be EXACTLY 0.0.
TEST(KWeightingFilterTest, FeedbackStateFlushesToExactZeroOnSilence) {
    auto filter = KWeightingFilter::create(stereo48k());
    ASSERT_TRUE(filter.hasValue());

    // Excite both channels so their biquad state is well away from zero.
    for (int i = 0; i < 4'800; ++i) {
        const float x = std::sin(0.05F * static_cast<float>(i));
        (void)filter.value().processSample(0, x);
        (void)filter.value().processSample(1, 0.5F * x);
    }
    ASSERT_GT(filter.value().maxAbsState(), 0.0);

    // 1 s of exact silence per channel: the flush must collapse the slow decay.
    for (int i = 0; i < 48'000; ++i) {
        (void)filter.value().processSample(0, 0.0F);
        (void)filter.value().processSample(1, 0.0F);
    }

    EXPECT_EQ(filter.value().maxAbsState(), 0.0);  // exact, not a subnormal
}

TEST(KWeightingFilterTest, RejectsNon48kFormat) {
    auto filter = KWeightingFilter::create(AudioFormat::create(44'100, 2).value());
    ASSERT_FALSE(filter.hasValue());
    EXPECT_EQ(filter.error().code(), core::ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace creator::audio_dsp
