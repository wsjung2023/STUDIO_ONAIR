#include "avatar/ExpressionParameters.h"

#include <gtest/gtest.h>

namespace {

using creator::avatar::ExpressionParameters;

TEST(ExpressionParametersTest, EqualityComparesAllFields) {
    ExpressionParameters a{};
    a.eyeOpenLeft = 0.5F;
    a.eyeOpenRight = 0.6F;
    a.browUpLeft = 0.1F;
    a.browUpRight = 0.2F;
    a.mouthOpen = 0.3F;
    a.mouthWide = 0.4F;
    a.headYaw = -0.5F;
    a.headPitch = 0.25F;
    a.headRoll = -0.1F;

    ExpressionParameters b = a;
    EXPECT_EQ(a, b);

    // Changing any single field, including one deep in the struct, must break
    // equality - a hand-rolled operator== that only checked the first few
    // fields would still pass an equality test that only ever varied those
    // same first few fields.
    b.headRoll = a.headRoll + 0.01F;
    EXPECT_NE(a, b);
}

TEST(ExpressionParametersTest, NeutralIsTheDocumentedZeroPointForEveryField) {
    // The doc on the struct defines neutral as: eyes/brows/mouth at their
    // closed/neutral end of [0,1] (i.e. 0), head angles at 0 within [-1,1]
    // (facing the camera straight on). This pins neutral() to an explicitly
    // constructed value with the same meaning, not to whatever the default
    // member initializers happen to already say.
    ExpressionParameters explicitZero{};
    explicitZero.eyeOpenLeft = 0.0F;
    explicitZero.eyeOpenRight = 0.0F;
    explicitZero.browUpLeft = 0.0F;
    explicitZero.browUpRight = 0.0F;
    explicitZero.mouthOpen = 0.0F;
    explicitZero.mouthWide = 0.0F;
    explicitZero.headYaw = 0.0F;
    explicitZero.headPitch = 0.0F;
    explicitZero.headRoll = 0.0F;

    EXPECT_EQ(ExpressionParameters::neutral(), explicitZero);

    // And neutral must be distinguishable from a non-neutral expression -
    // otherwise every TrackingResult would look neutral regardless of input.
    ExpressionParameters expressive = explicitZero;
    expressive.mouthOpen = 0.8F;
    EXPECT_NE(ExpressionParameters::neutral(), expressive);
}

}  // namespace
