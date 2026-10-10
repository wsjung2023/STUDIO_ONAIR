#include "avatar/AudioLipSyncTrackingProvider.h"

#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

namespace {

using creator::avatar::AudioLipSyncTrackingProvider;
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

// Drive the provider at 30 fps from `from` to `to` seconds; returns the last
// result.
creator::avatar::TrackingResult run(AudioLipSyncTrackingProvider& provider, double from,
                                    double to) {
    creator::avatar::TrackingResult last{};
    for (double t = from; t <= to; t += 1.0 / 30.0) {
        auto result = provider.process(frameAt(t));
        EXPECT_TRUE(result.hasValue());
        last = result.value();
    }
    return last;
}

TEST(AudioLipSyncTrackingProviderTest, ReportsAudioLipSyncIdentity) {
    AudioLipSyncTrackingProvider provider;
    EXPECT_EQ(provider.providerId().value(), AudioLipSyncTrackingProvider::kProviderId);
    const auto result = provider.process(frameAt(0.5));
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().faceFound);
    EXPECT_FLOAT_EQ(result.value().confidence, 1.0F);
    EXPECT_EQ(result.value().timestamp, frameAt(0.5).timestamp);
}

// With no voice the mouth stays shut, but the character is not a dead
// picture: it keeps breathing (a slow head motion) so the avatar reads as
// alive even before the creator speaks.
TEST(AudioLipSyncTrackingProviderTest, SilenceKeepsTheMouthClosedWhileIdleMotionContinues) {
    AudioLipSyncTrackingProvider provider;
    provider.setAudioLevelDbfs(-90.0F);
    float minPitch = 1.0F;
    float maxPitch = -1.0F;
    for (double t = 0.0; t <= 3.0; t += 1.0 / 30.0) {
        const auto result = provider.process(frameAt(t));
        ASSERT_TRUE(result.hasValue());
        EXPECT_LT(result.value().raw.mouthOpen, 0.05F);
        minPitch = std::min(minPitch, result.value().raw.headPitch);
        maxPitch = std::max(maxPitch, result.value().raw.headPitch);
    }
    EXPECT_GT(maxPitch - minPitch, 0.01F);
}

// Speaking opens the mouth within a few frames (fast attack) and silence
// closes it again a little more slowly (release), the way a real mouth does.
TEST(AudioLipSyncTrackingProviderTest, VoiceOpensTheMouthAndSilenceClosesIt) {
    AudioLipSyncTrackingProvider provider;
    provider.setAudioLevelDbfs(-90.0F);
    run(provider, 0.0, 0.5);
    provider.setAudioLevelDbfs(-18.0F);
    const auto talking = run(provider, 0.5, 0.8);
    EXPECT_GT(talking.raw.mouthOpen, 0.7F);
    provider.setAudioLevelDbfs(-90.0F);
    const auto justAfter = run(provider, 0.8, 0.85);
    EXPECT_GT(justAfter.raw.mouthOpen, 0.2F) << "release should not snap shut instantly";
    const auto later = run(provider, 0.85, 1.6);
    EXPECT_LT(later.raw.mouthOpen, 0.1F);
}

// Quiet room noise must not make the mouth flutter: below the gate the mouth
// stays closed; a moderate level opens it part way.
TEST(AudioLipSyncTrackingProviderTest, GatesRoomNoiseAndScalesWithLevel) {
    AudioLipSyncTrackingProvider provider;
    provider.setAudioLevelDbfs(AudioLipSyncTrackingProvider::kSilenceGateDbfs - 5.0F);
    EXPECT_LT(run(provider, 0.0, 1.0).raw.mouthOpen, 0.05F);
    const float mid = (AudioLipSyncTrackingProvider::kSilenceGateDbfs +
                       AudioLipSyncTrackingProvider::kFullOpenDbfs) / 2.0F;
    provider.setAudioLevelDbfs(mid);
    const auto half = run(provider, 1.0, 2.0).raw.mouthOpen;
    EXPECT_GT(half, 0.3F);
    EXPECT_LT(half, 0.7F);
}

// Eyes blink on their own: mostly open, briefly closed, both eyes together.
TEST(AudioLipSyncTrackingProviderTest, BlinksPeriodicallyWithBothEyes) {
    AudioLipSyncTrackingProvider provider;
    int openFrames = 0;
    int closedFrames = 0;
    for (double t = 0.0; t <= 10.0; t += 1.0 / 60.0) {
        const auto result = provider.process(frameAt(t));
        ASSERT_TRUE(result.hasValue());
        EXPECT_FLOAT_EQ(result.value().raw.eyeOpenLeft, result.value().raw.eyeOpenRight);
        if (result.value().raw.eyeOpenLeft > 0.9F) ++openFrames;
        if (result.value().raw.eyeOpenLeft < 0.2F) ++closedFrames;
    }
    EXPECT_GT(closedFrames, 2);
    EXPECT_GT(openFrames, closedFrames * 8);
}

TEST(AudioLipSyncTrackingProviderTest, ParametersStayWithinDocumentedRanges) {
    AudioLipSyncTrackingProvider provider;
    provider.setAudioLevelDbfs(0.0F);  // clipping-loud input must still clamp
    for (double t = 0.0; t <= 12.0; t += 0.07) {
        const auto result = provider.process(frameAt(t));
        ASSERT_TRUE(result.hasValue());
        const auto& p = result.value().raw;
        for (const float unit : {p.eyeOpenLeft, p.eyeOpenRight, p.browUpLeft, p.browUpRight,
                                 p.mouthOpen, p.mouthWide}) {
            EXPECT_GE(unit, 0.0F);
            EXPECT_LE(unit, 1.0F);
        }
        for (const float bipolar : {p.headYaw, p.headPitch, p.headRoll}) {
            EXPECT_GE(bipolar, -1.0F);
            EXPECT_LE(bipolar, 1.0F);
        }
    }
}

}  // namespace
