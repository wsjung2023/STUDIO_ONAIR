#include "avatar/ExpressionSmoother.h"

#include <cmath>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "avatar/ExpressionParameters.h"
#include "core/Timebase.h"

namespace {

using creator::avatar::ExpressionParameters;
using creator::avatar::ExpressionSmoother;
using creator::core::DurationNs;
using creator::core::TimestampNs;

constexpr float kTolerance = 1e-4F;

TimestampNs tsAtFrame(std::int64_t frameIndex, std::int64_t frameDurationNs) {
    return TimestampNs{DurationNs{frameIndex * frameDurationNs}};
}

double variance(const std::vector<float>& values) {
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double sumSquaredDeviation = 0.0;
    for (const float v : values) {
        const double d = static_cast<double>(v) - mean;
        sumSquaredDeviation += d * d;
    }
    return sumSquaredDeviation / static_cast<double>(values.size());
}

// A jittery sequence: a constant baseline plus alternating +-amplitude noise,
// sampled at a fixed frame rate. Real face-tracking jitter is exactly this
// shape at the scale of a single field - the true expression is still, but
// the tracker's per-frame estimate wobbles around it.
std::vector<float> jitterySequence(float baseline, float amplitude, int frameCount) {
    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(frameCount));
    for (int i = 0; i < frameCount; ++i) {
        values.push_back(baseline + ((i % 2 == 0) ? amplitude : -amplitude));
    }
    return values;
}

TEST(ExpressionSmootherTest, FirstSamplePassesThroughUnchanged) {
    ExpressionSmoother smoother;
    ExpressionParameters raw{};
    raw.eyeOpenLeft = 0.42F;
    raw.mouthOpen = 0.77F;
    raw.headYaw = -0.3F;

    const ExpressionParameters out = smoother.smooth(raw, tsAtFrame(0, 16'666'667));

    EXPECT_NEAR(out.eyeOpenLeft, raw.eyeOpenLeft, kTolerance);
    EXPECT_NEAR(out.mouthOpen, raw.mouthOpen, kTolerance);
    EXPECT_NEAR(out.headYaw, raw.headYaw, kTolerance);
}

TEST(ExpressionSmootherTest, JitterReductionVarianceIsMateriallySmallerThanInput) {
    constexpr std::int64_t kFrameDurationNs = 16'666'667;  // 60fps
    constexpr float kBaseline = 0.5F;
    constexpr float kAmplitude = 0.05F;
    constexpr int kFrameCount = 200;

    const std::vector<float> rawValues = jitterySequence(kBaseline, kAmplitude, kFrameCount);

    ExpressionSmoother smoother;
    std::vector<float> filteredValues;
    filteredValues.reserve(rawValues.size());
    for (int i = 0; i < kFrameCount; ++i) {
        ExpressionParameters raw{};
        raw.mouthOpen = rawValues[static_cast<std::size_t>(i)];
        const ExpressionParameters out = smoother.smooth(raw, tsAtFrame(i, kFrameDurationNs));
        filteredValues.push_back(out.mouthOpen);
    }

    // Skip the first few frames: the filter has a brief transient while xPrev
    // catches up to the baseline from the cold-start passthrough sample.
    constexpr std::size_t kWarmup = 20;
    const std::vector<float> rawTail(rawValues.begin() + kWarmup, rawValues.end());
    const std::vector<float> filteredTail(filteredValues.begin() + kWarmup, filteredValues.end());

    const double rawVariance = variance(rawTail);
    const double filteredVariance = variance(filteredTail);

    ASSERT_GT(rawVariance, 0.0);
    // The whole point of the filter: a still-but-jittery signal must come out
    // with materially less variance around the baseline. Measured for these
    // exact constants/inputs: rawVariance == 0.0025, filteredVariance ~=
    // 7.4e-6 (~340x reduction); asserting a 10x reduction leaves generous
    // margin while still failing hard for a no-op/near-passthrough filter.
    EXPECT_LT(filteredVariance, rawVariance * 0.1)
        << "rawVariance=" << rawVariance << " filteredVariance=" << filteredVariance;
}

TEST(ExpressionSmootherTest, JitterOutputIsNotPassthrough) {
    constexpr std::int64_t kFrameDurationNs = 16'666'667;
    constexpr float kBaseline = 0.5F;
    constexpr float kAmplitude = 0.05F;
    constexpr int kFrameCount = 30;

    const std::vector<float> rawValues = jitterySequence(kBaseline, kAmplitude, kFrameCount);

    ExpressionSmoother smoother;
    bool sawDifference = false;
    for (int i = 0; i < kFrameCount; ++i) {
        ExpressionParameters raw{};
        raw.mouthOpen = rawValues[static_cast<std::size_t>(i)];
        const ExpressionParameters out = smoother.smooth(raw, tsAtFrame(i, kFrameDurationNs));
        if (i > 0 && std::fabs(out.mouthOpen - raw.mouthOpen) > kTolerance) {
            sawDifference = true;
        }
    }

    EXPECT_TRUE(sawDifference) << "filter must actually change values past the first sample";
}

TEST(ExpressionSmootherTest, StepChangeConvergesWithinBoundedFramesAndIsMonotonic) {
    constexpr std::int64_t kFrameDurationNs = 16'666'667;  // 60fps
    constexpr float kOldValue = 0.1F;
    constexpr float kNewValue = 0.9F;
    constexpr int kSettleFrames = 200;
    // A converged threshold well inside the [kOldValue, kNewValue] span: not
    // 100% converged (a true low-pass never fully snaps), but "clearly
    // tracking the new value" rather than "frozen at the old one".
    constexpr float kConvergenceBand = 0.05F;

    ExpressionSmoother smoother;

    // Hold at the old value for a few frames so the filter is in steady
    // state before the step.
    ExpressionParameters lastOut{};
    for (int i = 0; i < 10; ++i) {
        ExpressionParameters raw{};
        raw.browUpLeft = kOldValue;
        lastOut = smoother.smooth(raw, tsAtFrame(i, kFrameDurationNs));
    }
    EXPECT_NEAR(lastOut.browUpLeft, kOldValue, 0.02F);

    float previous = lastOut.browUpLeft;
    int convergedAtFrame = -1;
    for (int i = 10; i < 10 + kSettleFrames; ++i) {
        ExpressionParameters raw{};
        raw.browUpLeft = kNewValue;
        const ExpressionParameters out = smoother.smooth(raw, tsAtFrame(i, kFrameDurationNs));

        // Monotonic toward the new (higher) value - no overshoot/oscillation
        // back down.
        EXPECT_GE(out.browUpLeft, previous - kTolerance)
            << "frame " << i << " output must not move away from the new value";
        previous = out.browUpLeft;

        if (convergedAtFrame < 0 && std::fabs(out.browUpLeft - kNewValue) < kConvergenceBand) {
            convergedAtFrame = i - 10;
        }
    }

    ASSERT_GE(convergedAtFrame, 0) << "output never converged toward the step's new value - "
                                       "filter is frozen (over-smoothed)";
    // Bounded lag: measured convergedAtFrame == 25 (~416ms at 60fps) for
    // these constants; assert well above that so a small constant retune
    // doesn't make this flaky, but still far inside the settle window so a
    // truly frozen filter (which would never converge) fails.
    EXPECT_LT(convergedAtFrame, kSettleFrames - 20);
}

TEST(ExpressionSmootherTest, DifferentFrameRatesProduceDifferentSmoothing) {
    constexpr float kOldValue = 0.1F;
    constexpr float kNewValue = 0.9F;

    auto runAtFrameRate = [](std::int64_t frameDurationNs) {
        ExpressionSmoother smoother;
        ExpressionParameters out{};
        for (int i = 0; i < 5; ++i) {
            ExpressionParameters raw{};
            raw.headPitch = kOldValue;
            out = smoother.smooth(raw, tsAtFrame(i, frameDurationNs));
        }
        for (int i = 5; i < 8; ++i) {
            ExpressionParameters raw{};
            raw.headPitch = kNewValue;
            out = smoother.smooth(raw, tsAtFrame(i, frameDurationNs));
        }
        return out.headPitch;
    };

    const float fastRateOutput = runAtFrameRate(16'666'667);   // 60fps
    const float slowRateOutput = runAtFrameRate(100'000'000);  // 10fps

    EXPECT_GT(std::fabs(fastRateOutput - slowRateOutput), kTolerance)
        << "fastRateOutput=" << fastRateOutput << " slowRateOutput=" << slowRateOutput;
}

TEST(ExpressionSmootherTest, NonPositiveDtDoesNotCrashOrProduceNanOrInf) {
    ExpressionSmoother smoother;

    ExpressionParameters first{};
    first.mouthOpen = 0.3F;
    const TimestampNs t0 = tsAtFrame(0, 16'666'667);
    const ExpressionParameters out0 = smoother.smooth(first, t0);
    EXPECT_TRUE(std::isfinite(out0.mouthOpen));

    ExpressionParameters second{};
    second.mouthOpen = 0.9F;

    // Repeated timestamp (dt == 0).
    const ExpressionParameters outRepeated = smoother.smooth(second, t0);
    EXPECT_TRUE(std::isfinite(outRepeated.mouthOpen));
    EXPECT_NEAR(outRepeated.mouthOpen, out0.mouthOpen, kTolerance)
        << "non-positive dt must return the last filtered output unchanged";

    // Out-of-order timestamp (dt < 0).
    const TimestampNs earlier = tsAtFrame(0, 16'666'667) - DurationNs{1'000'000};
    const ExpressionParameters outEarlier = smoother.smooth(second, earlier);
    EXPECT_TRUE(std::isfinite(outEarlier.mouthOpen));
    EXPECT_NEAR(outEarlier.mouthOpen, out0.mouthOpen, kTolerance);

    // A subsequent valid, forward timestamp must still work normally.
    const ExpressionParameters outForward =
        smoother.smooth(second, tsAtFrame(1, 16'666'667));
    EXPECT_TRUE(std::isfinite(outForward.mouthOpen));
}

TEST(ExpressionSmootherTest, ResetRestoresColdStartPassthrough) {
    ExpressionSmoother smoother;

    ExpressionParameters raw{};
    raw.eyeOpenLeft = 0.5F;
    static_cast<void>(smoother.smooth(raw, tsAtFrame(0, 16'666'667)));
    static_cast<void>(smoother.smooth(raw, tsAtFrame(1, 16'666'667)));

    smoother.reset();

    ExpressionParameters next{};
    next.eyeOpenLeft = 0.9F;
    const ExpressionParameters out = smoother.smooth(next, tsAtFrame(0, 16'666'667));

    EXPECT_NEAR(out.eyeOpenLeft, next.eyeOpenLeft, kTolerance)
        << "after reset(), the next sample must be a fresh cold-start passthrough";
}

TEST(ExpressionSmootherTest, PerFieldIndependenceAJumpInOneFieldDoesNotMoveAnother) {
    constexpr std::int64_t kFrameDurationNs = 16'666'667;

    ExpressionSmoother smoother;

    ExpressionParameters raw{};
    raw.mouthOpen = 0.2F;
    raw.headYaw = 0.0F;
    static_cast<void>(smoother.smooth(raw, tsAtFrame(0, kFrameDurationNs)));
    for (int i = 1; i < 10; ++i) {
        static_cast<void>(smoother.smooth(raw, tsAtFrame(i, kFrameDurationNs)));
    }

    // Jump mouthOpen only; headYaw stays at its steady, unchanging value.
    ExpressionParameters jumped{};
    jumped.mouthOpen = 0.95F;
    jumped.headYaw = 0.0F;
    const ExpressionParameters out = smoother.smooth(jumped, tsAtFrame(10, kFrameDurationNs));

    EXPECT_GT(out.mouthOpen, raw.mouthOpen)
        << "mouthOpen must move in response to its own jump";
    EXPECT_NEAR(out.headYaw, 0.0F, kTolerance)
        << "headYaw must be unaffected by mouthOpen's jump";
}

}  // namespace
