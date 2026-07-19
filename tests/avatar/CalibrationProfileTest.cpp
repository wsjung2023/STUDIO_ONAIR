#include "avatar/CalibrationProfile.h"

#include <cmath>
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

    // Offset-removal calibration: clamp(raw - baseline, lo, hi) ==
    // clamp(0.9 - 0.15, 0, 1) == 0.75. No rescale/gain in Stage A - see
    // CalibrationProfile.h.
    EXPECT_GT(calibrated.mouthOpen, 0.0F);
    EXPECT_NEAR(calibrated.mouthOpen, 0.75F, kTolerance);
}

TEST(CalibrationProfileTest, BelowNeutralUnitFieldClampsToZeroByDesign) {
    // eye/brow/mouth are documented as unidirectional expression magnitude
    // (0 = neutral, 1 = max): a raw reading below the performer's own
    // captured neutral still means "no expression", not a negative
    // expression, so it must clamp to exactly 0 - not collapse through some
    // broken degenerate-denominator branch, and not read as a negative
    // value either. This is the intended semantics of offset+clamp
    // calibration on a [0,1] field, documented in CalibrationProfile.h.
    ExpressionParameters neutralRaw{};
    neutralRaw.mouthOpen = 0.4F; // this performer's resting mouth reads 0.4, not 0

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());
    const CalibrationProfile profile = profileResult.value();

    for (const float belowNeutral : {0.35F, 0.2F, 0.0F}) {
        ExpressionParameters raw = neutralRaw;
        raw.mouthOpen = belowNeutral;

        const ExpressionParameters calibrated = profile.apply(raw);

        EXPECT_NEAR(calibrated.mouthOpen, 0.0F, kTolerance)
            << "raw.mouthOpen=" << belowNeutral << " below neutral 0.4 must clamp to 0";
    }
}

TEST(CalibrationProfileTest, BelowNeutralHeadFieldStaysGradedNotCollapsed) {
    // headYaw is bidirectional over [-1,1] with 0 in the interior of the
    // range (not at an endpoint), so unlike a [0,1] expression-magnitude
    // field, a below-neutral raw must remain a distinct, graded negative
    // value per distinct input - proving head fields are not collapsed to a
    // single "no signal" value the way [0,1] fields intentionally are.
    ExpressionParameters neutralRaw{};
    neutralRaw.headYaw = 0.1F; // camera mounted slightly off-axis

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());
    const CalibrationProfile profile = profileResult.value();

    ExpressionParameters rawA = neutralRaw;
    rawA.headYaw = -0.2F; // below this performer's own captured neutral
    ExpressionParameters rawB = neutralRaw;
    rawB.headYaw = -0.5F; // further below

    const ExpressionParameters calibratedA = profile.apply(rawA);
    const ExpressionParameters calibratedB = profile.apply(rawB);

    EXPECT_NEAR(calibratedA.headYaw, -0.3F, kTolerance);
    EXPECT_NEAR(calibratedB.headYaw, -0.6F, kTolerance);
    EXPECT_LT(calibratedB.headYaw, calibratedA.headYaw)
        << "two distinct below-neutral raw values must yield two distinct outputs, not both "
           "collapse to the same value";
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

TEST(CalibrationProfileTest, FromNeutralRejectsOutOfRangeButFiniteBaseline) {
    // A finite baseline that is nonetheless outside the field's own
    // documented range (e.g. mouthOpen == 1.4, but mouthOpen is documented
    // [0,1]) is a malformed capture, not a valid "expressive" neutral pose -
    // it must be rejected the same way a NaN/infinite baseline is.
    ExpressionParameters neutralRaw{};
    neutralRaw.mouthOpen = 1.4F; // finite, but beyond documented [0,1] max

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);

    ASSERT_FALSE(profileResult.hasValue());
    EXPECT_EQ(profileResult.error().code(), ErrorCode::InvalidArgument);
}

TEST(CalibrationProfileTest, ApplySanitizesNanRawFieldToNeutralWithoutAffectingOtherFields) {
    // A non-finite raw field must not leak through the clamp as NaN/Inf
    // (std::clamp(NaN, lo, hi) returns NaN, since both comparisons are
    // false) - CLAUDE.md 9 forbids hiding a tracking failure behind a
    // fake-valid number. The provider must signal tracking loss via
    // faceFound/confidence, not NaN; apply() defensively treats a NaN field
    // as that field's neutral value.
    ExpressionParameters neutralRaw{};
    neutralRaw.mouthOpen = 0.2F;

    const auto profileResult = CalibrationProfile::fromNeutral(neutralRaw);
    ASSERT_TRUE(profileResult.hasValue());
    const CalibrationProfile profile = profileResult.value();

    ExpressionParameters raw = neutralRaw;
    raw.mouthOpen = std::numeric_limits<float>::quiet_NaN();
    raw.eyeOpenLeft = 0.6F; // an ordinary, unaffected field

    const ExpressionParameters calibrated = profile.apply(raw);

    EXPECT_TRUE(std::isfinite(calibrated.mouthOpen));
    EXPECT_NEAR(calibrated.mouthOpen, 0.0F, kTolerance);
    EXPECT_NEAR(calibrated.eyeOpenLeft, 0.6F, kTolerance);
}

}  // namespace
