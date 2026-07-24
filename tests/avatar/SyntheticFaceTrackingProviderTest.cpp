#include "avatar/SyntheticFaceTrackingProvider.h"

#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using creator::avatar::SyntheticFaceTrackingProvider;
using creator::core::TimestampNs;

creator::media::VideoFrame frameAt(double seconds) {
    creator::media::VideoFrame frame{};
    frame.timestamp =
        TimestampNs{} + std::chrono::duration_cast<creator::core::DurationNs>(
                            std::chrono::duration<double>(seconds));
    frame.width = 640;
    frame.height = 480;
    frame.pixelFormat = creator::media::PixelFormat::Bgra8;
    return frame;
}

TEST(SyntheticFaceTrackingProviderTest, ReportsSyntheticProviderIdentity) {
    SyntheticFaceTrackingProvider provider;
    EXPECT_EQ(provider.providerId().value(),
              SyntheticFaceTrackingProvider::kProviderId);
}

TEST(SyntheticFaceTrackingProviderTest, AlwaysFindsAFaceWithFullConfidence) {
    SyntheticFaceTrackingProvider provider;
    const auto result = provider.process(frameAt(1.0));
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().faceFound);
    EXPECT_FLOAT_EQ(result.value().confidence, 1.0F);
    EXPECT_EQ(result.value().timestamp, frameAt(1.0).timestamp);
}

TEST(SyntheticFaceTrackingProviderTest, ParametersStayWithinDocumentedRanges) {
    SyntheticFaceTrackingProvider provider;
    for (double t = 0.0; t < 8.0; t += 0.05) {
        const auto result = provider.process(frameAt(t));
        ASSERT_TRUE(result.hasValue());
        const auto& p = result.value().raw;
        for (float value : {p.eyeOpenLeft, p.eyeOpenRight, p.browUpLeft,
                            p.browUpRight, p.mouthOpen, p.mouthWide}) {
            EXPECT_GE(value, 0.0F);
            EXPECT_LE(value, 1.0F);
        }
        for (float value : {p.headYaw, p.headPitch, p.headRoll}) {
            EXPECT_GE(value, -1.0F);
            EXPECT_LE(value, 1.0F);
        }
    }
}

TEST(SyntheticFaceTrackingProviderTest, IsDeterministicForEqualTimestamps) {
    SyntheticFaceTrackingProvider a;
    SyntheticFaceTrackingProvider b;
    const auto first = a.process(frameAt(2.345));
    const auto second = b.process(frameAt(2.345));
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().raw, second.value().raw);
}

TEST(SyntheticFaceTrackingProviderTest, ExpressionActuallyMovesOverTime) {
    SyntheticFaceTrackingProvider provider;
    const auto a = provider.process(frameAt(0.0));
    const auto b = provider.process(frameAt(0.75));
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    // Mouth and head should change across time so the avatar visibly moves.
    EXPECT_NE(a.value().raw, b.value().raw);
}

}  // namespace
