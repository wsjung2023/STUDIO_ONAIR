#include "avatar/CalibrationCapture.h"

#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "avatar/CalibrationProfile.h"
#include "avatar/ExpressionNormalizer.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/TrackingResult.h"
#include "core/AppError.h"

namespace {

using creator::avatar::CalibrationCapture;
using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionNormalizer;
using creator::avatar::ExpressionParameters;
using creator::avatar::TrackingResult;
using creator::core::ErrorCode;

constexpr float kTolerance = 1e-4F;

TrackingResult goodFrame(float mouthOpen, float eyeOpenLeft = 0.0F) {
    TrackingResult result{};
    result.faceFound = true;
    result.confidence = 1.0F;
    result.raw.mouthOpen = mouthOpen;
    result.raw.eyeOpenLeft = eyeOpenLeft;
    return result;
}

TEST(CalibrationCaptureTest, MinCalibrationFramesIsDocumented) {
    // Pins the documented floor itself, not just its effect - if the
    // constant drifts, this test should visibly fail.
    EXPECT_EQ(CalibrationCapture::kMinCalibrationFrames, 10U);
}

TEST(CalibrationCaptureTest, MinConfidenceMatchesExpressionNormalizersFloor) {
    // CalibrationCapture deliberately reuses ExpressionNormalizer's
    // confidence floor rather than defining an independently-tunable one -
    // pin that they stay equal.
    EXPECT_NEAR(CalibrationCapture::kMinConfidence, ExpressionNormalizer::kMinConfidence,
                kTolerance);
}

TEST(CalibrationCaptureTest, BuildFailsWithTooFewAcceptedFrames) {
    CalibrationCapture capture;
    // 3 good frames, well below kMinCalibrationFrames (10).
    capture.add(goodFrame(0.1F));
    capture.add(goodFrame(0.1F));
    capture.add(goodFrame(0.1F));

    EXPECT_EQ(capture.acceptedCount(), 3U);

    const auto result = capture.build();

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CalibrationCaptureTest, BuildSucceedsWithEnoughCleanNeutralFramesAndZeroesThatNeutral) {
    CalibrationCapture capture;
    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(goodFrame(/*mouthOpen=*/0.2F, /*eyeOpenLeft=*/0.35F));
    }
    ASSERT_EQ(capture.acceptedCount(), CalibrationCapture::kMinCalibrationFrames);

    const auto result = capture.build();
    ASSERT_TRUE(result.hasValue());

    // Applying the captured profile to that exact same neutral pose must
    // read back as neutral (0) end to end - this proves the accumulated
    // baseline is the correct one, not just "some" baseline.
    ExpressionParameters sameNeutral{};
    sameNeutral.mouthOpen = 0.2F;
    sameNeutral.eyeOpenLeft = 0.35F;
    const ExpressionParameters calibrated = result.value().apply(sameNeutral);

    EXPECT_NEAR(calibrated.mouthOpen, 0.0F, kTolerance);
    EXPECT_NEAR(calibrated.eyeOpenLeft, 0.0F, kTolerance);
}

TEST(CalibrationCaptureTest, OutlierFramesDoNotMoveTheMedianBaseline) {
    // The point of the whole class: a handful of wildly-off frames among a
    // majority of clean neutral frames must not drag the baseline toward
    // them. 10 clean frames at mouthOpen=0.1 plus 3 outliers at
    // mouthOpen=0.9: sorted, the outliers sit at the high end of the 13
    // accepted values and the median (7th of 13, index 6) is still 0.1 -
    // whereas the MEAN would be (10*0.1 + 3*0.9)/13 ~= 0.2846, clearly
    // dragged toward the outliers. This test asserts the median behaviour,
    // not the mean.
    CalibrationCapture capture;
    for (int i = 0; i < 10; ++i) {
        capture.add(goodFrame(/*mouthOpen=*/0.1F));
    }
    for (int i = 0; i < 3; ++i) {
        capture.add(goodFrame(/*mouthOpen=*/0.9F));
    }
    ASSERT_EQ(capture.acceptedCount(), 13U);

    const auto result = capture.build();
    ASSERT_TRUE(result.hasValue());

    ExpressionParameters probe{};
    probe.mouthOpen = 0.1F; // the expected (unmoved) median baseline
    const ExpressionParameters calibrated = result.value().apply(probe);

    // If the baseline had been the mean (~0.2846) this would read strongly
    // negative-before-clamp, i.e. clamp to 0 same as a correct baseline -
    // so also directly probe a value between the two candidate baselines
    // to distinguish them unambiguously.
    EXPECT_NEAR(calibrated.mouthOpen, 0.0F, kTolerance)
        << "median baseline (0.1) applied to the clean-neutral value must read exactly neutral";

    ExpressionParameters midProbe{};
    midProbe.mouthOpen = 0.2F; // between the median (0.1) and the mean (~0.2846)
    const ExpressionParameters calibratedMid = result.value().apply(midProbe);

    // clamp(0.2 - baseline, 0, 1): with the correct median baseline (0.1)
    // this is 0.1 (clearly positive); with a mean-dragged baseline (~0.2846)
    // this would clamp to 0 instead. A positive result here proves the
    // baseline was NOT dragged toward the outliers.
    EXPECT_NEAR(calibratedMid.mouthOpen, 0.1F, kTolerance)
        << "baseline must be the median (0.1), not a mean dragged toward the outliers (~0.2846)";
}

TEST(CalibrationCaptureTest, MedianOfOddAcceptedCountUsesTheMiddleValue) {
    // 11 distinct eyeOpenLeft values (0.05..0.55 step 0.05), added out of
    // sorted order to prove build() sorts rather than relying on insertion
    // order. Sorted, the middle (6th of 11, 0-based index 5) value is 0.30.
    const std::vector<float> values{0.30F, 0.05F, 0.45F, 0.15F, 0.55F, 0.10F,
                                     0.40F, 0.20F, 0.50F, 0.25F, 0.35F};
    ASSERT_EQ(values.size(), 11U);

    CalibrationCapture capture;
    for (const float v : values) {
        capture.add(goodFrame(/*mouthOpen=*/0.0F, /*eyeOpenLeft=*/v));
    }
    ASSERT_EQ(capture.acceptedCount(), 11U);

    const auto result = capture.build();
    ASSERT_TRUE(result.hasValue());

    ExpressionParameters probe{};
    probe.eyeOpenLeft = 0.30F; // expected median
    const ExpressionParameters calibrated = result.value().apply(probe);
    EXPECT_NEAR(calibrated.eyeOpenLeft, 0.0F, kTolerance);

    ExpressionParameters probeAbove{};
    probeAbove.eyeOpenLeft = 0.50F; // median + 0.2
    const ExpressionParameters calibratedAbove = result.value().apply(probeAbove);
    EXPECT_NEAR(calibratedAbove.eyeOpenLeft, 0.20F, kTolerance);
}

TEST(CalibrationCaptureTest, MedianOfEvenAcceptedCountAveragesTheTwoMiddleValues) {
    // 10 distinct eyeOpenLeft values (0.05..0.50 step 0.05), out of sorted
    // order. Sorted, the two middle values (5th and 6th of 10) are 0.25 and
    // 0.30; the documented even-count rule averages them to 0.275.
    const std::vector<float> values{0.30F, 0.05F, 0.45F, 0.15F, 0.10F,
                                     0.40F, 0.20F, 0.50F, 0.25F, 0.35F};
    ASSERT_EQ(values.size(), 10U);

    CalibrationCapture capture;
    for (const float v : values) {
        capture.add(goodFrame(/*mouthOpen=*/0.0F, /*eyeOpenLeft=*/v));
    }
    ASSERT_EQ(capture.acceptedCount(), 10U);

    const auto result = capture.build();
    ASSERT_TRUE(result.hasValue());

    ExpressionParameters probe{};
    probe.eyeOpenLeft = 0.275F; // expected averaged median
    const ExpressionParameters calibrated = result.value().apply(probe);
    EXPECT_NEAR(calibrated.eyeOpenLeft, 0.0F, kTolerance);
}

TEST(CalibrationCaptureTest, FaceNotFoundFramesAreRejectedAndNotCounted) {
    CalibrationCapture capture;
    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(goodFrame(0.1F));
    }

    TrackingResult noFace{};
    noFace.faceFound = false;
    noFace.confidence = 1.0F;
    noFace.raw.mouthOpen = 0.1F; // otherwise perfectly clean

    capture.add(noFace);
    capture.add(noFace);

    EXPECT_EQ(capture.acceptedCount(), CalibrationCapture::kMinCalibrationFrames);
    EXPECT_EQ(capture.rejectedCount(), 2U);
}

TEST(CalibrationCaptureTest, NonFiniteRawFramesAreRejectedAndNotCounted) {
    CalibrationCapture capture;
    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(goodFrame(0.1F));
    }

    TrackingResult nanFrame = goodFrame(0.1F);
    nanFrame.raw.headYaw = std::numeric_limits<float>::quiet_NaN();

    TrackingResult infFrame = goodFrame(0.1F);
    infFrame.raw.headPitch = std::numeric_limits<float>::infinity();

    capture.add(nanFrame);
    capture.add(infFrame);

    EXPECT_EQ(capture.acceptedCount(), CalibrationCapture::kMinCalibrationFrames);
    EXPECT_EQ(capture.rejectedCount(), 2U);
}

TEST(CalibrationCaptureTest, LowConfidenceFramesAreRejectedAndNotCounted) {
    CalibrationCapture capture;
    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(goodFrame(0.1F));
    }

    TrackingResult lowConfidence = goodFrame(0.1F);
    lowConfidence.confidence = CalibrationCapture::kMinConfidence - 0.01F;

    capture.add(lowConfidence);

    EXPECT_EQ(capture.acceptedCount(), CalibrationCapture::kMinCalibrationFrames);
    EXPECT_EQ(capture.rejectedCount(), 1U);
}

TEST(CalibrationCaptureTest, AtThresholdConfidenceFrameIsAccepted) {
    CalibrationCapture capture;
    TrackingResult atThreshold = goodFrame(0.1F);
    atThreshold.confidence = CalibrationCapture::kMinConfidence; // exactly at, not below

    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(atThreshold);
    }

    EXPECT_EQ(capture.acceptedCount(), CalibrationCapture::kMinCalibrationFrames);
    EXPECT_EQ(capture.rejectedCount(), 0U);
}

TEST(CalibrationCaptureTest, ResetClearsAccumulationAndCounters) {
    CalibrationCapture capture;
    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(goodFrame(0.1F));
    }
    TrackingResult noFace{};
    noFace.faceFound = false;
    capture.add(noFace);

    ASSERT_EQ(capture.acceptedCount(), CalibrationCapture::kMinCalibrationFrames);
    ASSERT_EQ(capture.rejectedCount(), 1U);
    ASSERT_TRUE(capture.build().hasValue());

    capture.reset();

    EXPECT_EQ(capture.acceptedCount(), 0U);
    EXPECT_EQ(capture.rejectedCount(), 0U);

    const auto result = capture.build();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);

    // A fresh capture after reset() must still work normally.
    for (std::size_t i = 0; i < CalibrationCapture::kMinCalibrationFrames; ++i) {
        capture.add(goodFrame(0.3F));
    }
    EXPECT_TRUE(capture.build().hasValue());
}

}  // namespace
