#include "avatar/CalibrationProfile.h"

#include <limits>

#include <gtest/gtest.h>

#include "core/AppError.h"

namespace {

using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionParameters;
using creator::core::ErrorCode;

constexpr float kTolerance = 1e-4F;

TEST(CalibrationProfileTest, IdentityPassesInRangeValuesThroughUnchanged) {
    ExpressionParameters raw{};
    raw.eyeOpenLeft = 0.4F;
    raw.eyeOpenRight = 0.7F;
    raw.browUpLeft = 0.2F;
    raw.browUpRight = 0.9F;
    raw.mouthOpen = 0.55F;
    raw.mouthWide = 0.1F;
    raw.headYaw = -0.6F;
    raw.headPitch = 0.3F;
    raw.headRoll = -0.15F;

    const ExpressionParameters calibrated = CalibrationProfile::identity().apply(raw);

    EXPECT_NEAR(calibrated.eyeOpenLeft, raw.eyeOpenLeft, kTolerance);
    EXPECT_NEAR(calibrated.eyeOpenRight, raw.eyeOpenRight, kTolerance);
    EXPECT_NEAR(calibrated.browUpLeft, raw.browUpLeft, kTolerance);
    EXPECT_NEAR(calibrated.browUpRight, raw.browUpRight, kTolerance);
    EXPECT_NEAR(calibrated.mouthOpen, raw.mouthOpen, kTolerance);
    EXPECT_NEAR(calibrated.mouthWide, raw.mouthWide, kTolerance);
    EXPECT_NEAR(calibrated.headYaw, raw.headYaw, kTolerance);
    EXPECT_NEAR(calibrated.headPitch, raw.headPitch, kTolerance);
    EXPECT_NEAR(calibrated.headRoll, raw.headRoll, kTolerance);
}

TEST(CalibrationProfileTest, IdentityClampsOutOfRangeRawToDocumentedRange) {
    ExpressionParameters raw{};
    raw.mouthOpen = 1.8F;    // above documented [0,1] max
    raw.eyeOpenLeft = -0.5F; // below documented [0,1] min
    raw.headYaw = 2.0F;      // above documented [-1,1] max
    raw.headPitch = -3.0F;   // below documented [-1,1] min

    const ExpressionParameters calibrated = CalibrationProfile::identity().apply(raw);

    EXPECT_NEAR(calibrated.mouthOpen, 1.0F, kTolerance);
    EXPECT_NEAR(calibrated.eyeOpenLeft, 0.0F, kTolerance);
    EXPECT_NEAR(calibrated.headYaw, 1.0F, kTolerance);
    EXPECT_NEAR(calibrated.headPitch, -1.0F, kTolerance);
}

TEST(CalibrationProfileTest, FromNeutralThenApplyOnTheSameNeutralYieldsNeutral) {
    ExpressionParameters neutralRaw{};
    neutralRaw.eyeOpenLeft = 0.35F;
    neutralRaw.eyeOpenRight = 0.4F;
    neutralRaw.browUpLeft = 0.2F;
    neutralRaw.browUpRight = 0.25F;
    neutralRaw.mouthOpen = 0.1F;
    neutralRaw.mouthWide = 0.3F;
    neutralRaw.headYaw = 0.05F;
    neutralRaw.headPitch = -0.1F;
    neutralRaw.headRoll = 0.02F;

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());

    const ExpressionParameters calibratedNeutral = profileResult.value().apply(neutralRaw);

    // Every field's own captured baseline must map back to that field's
    // documented zero - if it didn't, a performer's ordinary resting face
    // would read as an expression rather than as neutral.
    EXPECT_NEAR(calibratedNeutral.eyeOpenLeft, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.eyeOpenRight, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.browUpLeft, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.browUpRight, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.mouthOpen, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.mouthWide, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.headYaw, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.headPitch, 0.0F, kTolerance);
    EXPECT_NEAR(calibratedNeutral.headRoll, 0.0F, kTolerance);
}

TEST(CalibrationProfileTest, ExpressiveInputMapsAboveTheCalibratedNeutral) {
    ExpressionParameters neutralRaw{};
    neutralRaw.mouthOpen = 0.15F; // this performer's resting mouth reads 0.15, not 0

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());
    const CalibrationProfile profile = profileResult.value();

    ExpressionParameters expressive = neutralRaw;
    expressive.mouthOpen = 0.9F; // performer opens their mouth wide

    const ExpressionParameters calibrated = profile.apply(expressive);

    // (0.9 - 0.15) / (1 - 0.15) * 1 == 0.75 / 0.85 - the rescale step
    // stretching the remaining travel back onto the full documented range.
    EXPECT_GT(calibrated.mouthOpen, 0.0F);
    EXPECT_NEAR(calibrated.mouthOpen, 0.75F / 0.85F, kTolerance);
}

TEST(CalibrationProfileTest, FromNeutralClampsOutOfRangeInputAfterCalibration) {
    ExpressionParameters neutralRaw{};
    neutralRaw.mouthOpen = 0.1F;

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());

    ExpressionParameters raw = neutralRaw;
    raw.mouthOpen = 1.6F; // beyond the documented [0,1] max even before calibration

    const ExpressionParameters calibrated = profileResult.value().apply(raw);

    EXPECT_NEAR(calibrated.mouthOpen, 1.0F, kTolerance);
}

TEST(CalibrationProfileTest, FromNeutralRejectsNanBaseline) {
    ExpressionParameters neutralRaw{};
    neutralRaw.mouthOpen = std::numeric_limits<float>::quiet_NaN();

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);

    ASSERT_FALSE(profileResult.hasValue());
    EXPECT_EQ(profileResult.error().code(), ErrorCode::InvalidArgument);
}

TEST(CalibrationProfileTest, FromNeutralRejectsInfiniteBaseline) {
    ExpressionParameters neutralRaw{};
    neutralRaw.headYaw = std::numeric_limits<float>::infinity();

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);

    ASSERT_FALSE(profileResult.hasValue());
    EXPECT_EQ(profileResult.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
