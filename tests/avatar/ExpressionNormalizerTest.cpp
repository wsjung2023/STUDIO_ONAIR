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
    // High confidence: this test intends to exercise the calibrated path, not
    // the confidence gate, so it must clear the default threshold.
    result.confidence = 1.0F;
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
    // High confidence: this test intends to exercise the calibrated+clamped
    // path, not the confidence gate.
    result.confidence = 1.0F;
    result.raw.headPitch = 5.0F; // far beyond documented [-1,1]

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    EXPECT_NEAR(normalized.headPitch, 1.0F, kTolerance);
}

TEST(ExpressionNormalizerTest, DefaultMinConfidenceIsDocumented) {
    // Pins the documented default threshold value itself, not just its
    // effect - if the constant drifts, this test (not just behavior tests)
    // should visibly fail.
    EXPECT_NEAR(ExpressionNormalizer::kMinConfidence, 0.15F, kTolerance);
}

TEST(ExpressionNormalizerTest, BelowThresholdConfidenceYieldsNeutralEvenWhenFaceFound) {
    TrackingResult result{};
    result.faceFound = true;
    result.confidence = ExpressionNormalizer::kMinConfidence - 0.01F; // just below
    // Clearly non-neutral raw so a leaked reading would be caught.
    result.raw.mouthOpen = 0.8F;
    result.raw.headYaw = 0.5F;

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    EXPECT_EQ(normalized, ExpressionParameters::neutral());
}

TEST(ExpressionNormalizerTest, AtOrAboveThresholdConfidenceUsesCalibratedPath) {
    TrackingResult result{};
    result.faceFound = true;
    result.confidence = ExpressionNormalizer::kMinConfidence; // exactly at threshold
    result.raw.mouthOpen = 0.6F;

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    // Must NOT be forced to neutral: at-threshold is trusted.
    EXPECT_NEAR(normalized.mouthOpen, 0.6F, kTolerance);
}

TEST(ExpressionNormalizerTest, HighConfidenceDoesNotOverrideFaceNotFoundGate) {
    TrackingResult result{};
    result.faceFound = false;
    result.confidence = 1.0F; // maximal confidence, but no face at all
    result.raw.mouthOpen = 0.9F;

    const ExpressionNormalizer normalizer(CalibrationProfile::identity());
    const ExpressionParameters normalized = normalizer.normalize(result);

    EXPECT_EQ(normalized, ExpressionParameters::neutral());
}

TEST(ExpressionNormalizerTest, CustomMinConfidenceChangesWhichFramesPass) {
    TrackingResult result{};
    result.faceFound = true;
    result.confidence = 0.3F; // mid-range: between a lax and a strict threshold
    result.raw.mouthOpen = 0.6F;

    const ExpressionNormalizer lax(CalibrationProfile::identity(), /*minConfidence=*/0.1F);
    const ExpressionNormalizer strict(CalibrationProfile::identity(), /*minConfidence=*/0.5F);

    const ExpressionParameters laxResult = lax.normalize(result);
    const ExpressionParameters strictResult = strict.normalize(result);

    // The same mid-confidence frame must be treated differently by the two
    // normalizers: lax accepts it (calibrated path), strict gates it to
    // neutral.
    EXPECT_NEAR(laxResult.mouthOpen, 0.6F, kTolerance);
    EXPECT_EQ(strictResult, ExpressionParameters::neutral());
}

}  // namespace
