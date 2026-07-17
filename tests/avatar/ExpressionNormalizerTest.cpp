#include "avatar/ExpressionNormalizer.h"

#include <gtest/gtest.h>

#include "avatar/CalibrationProfile.h"

namespace {

using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionNormalizer;
using creator::avatar::ExpressionParameters;
using creator::avatar::TrackingResult;

constexpr float kTolerance = 1e-4F;

TEST(ExpressionNormalizerTest, NormalizeAppliesIdentityCalibrationWhenFaceFound) {
    TrackingResult result{};
    result.faceFound = true;
    result.confidence = 0.9F;
    result.raw.mouthOpen = 0.6F;
    result.raw.headYaw = -0.3F;

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    EXPECT_NEAR(normalized.mouthOpen, 0.6F, kTolerance);
    EXPECT_NEAR(normalized.headYaw, -0.3F, kTolerance);
}

TEST(ExpressionNormalizerTest, NormalizeAppliesTheCalibratedBaselineNotJustIdentity) {
    ExpressionParameters neutralRaw{};
    neutralRaw.eyeOpenLeft = 0.2F; // this performer's resting eye reads 0.2

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());

    TrackingResult result{};
    result.faceFound = true;
    result.raw.eyeOpenLeft = 0.2F; // exactly at this performer's own neutral

    const ExpressionNormalizer normalizer(profileResult.value());
    const ExpressionParameters normalized = normalizer.normalize(result);

    // Without calibration this would read 0.2 (non-neutral); with the
    // performer's own baseline applied it must read as neutral (0).
    EXPECT_NEAR(normalized.eyeOpenLeft, 0.0F, kTolerance);
}

TEST(ExpressionNormalizerTest, FaceNotFoundYieldsNeutralRegardlessOfRawExpression) {
    TrackingResult result{};
    result.faceFound = false;
    // A large, clearly-nonzero raw expression that must NOT leak through -
    // this is the "no stale expression leaks through" requirement.
    result.raw.mouthOpen = 0.95F;
    result.raw.eyeOpenLeft = 0.8F;
    result.raw.headYaw = 0.7F;
    result.confidence = 0.0F;

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    EXPECT_EQ(normalized, ExpressionParameters::neutral());
}

TEST(ExpressionNormalizerTest, NormalizeClampsCalibratedResultToDocumentedRange) {
    TrackingResult result{};
    result.faceFound = true;
    result.raw.headPitch = 5.0F; // far beyond documented [-1,1]

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    EXPECT_NEAR(normalized.headPitch, 1.0F, kTolerance);
}

}  // namespace
