#include "fakes/FakeTrackingProvider.h"

#include "avatar/AvatarProviderId.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/TrackingResult.h"
#include "capture/ICaptureSource.h"
#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "fakes/FakeCaptureSource.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using creator::avatar::AvatarProviderId;
using creator::avatar::ExpressionParameters;
using creator::avatar::TrackingResult;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::domain::SourceId;
using creator::fakes::FakeCaptureSource;
using creator::fakes::FakeTrackingProvider;

creator::media::VideoFrame makeFrame(std::int64_t timestampNs) {
    creator::media::VideoFrame frame{};
    frame.timestamp = TimestampNs{DurationNs{timestampNs}};
    return frame;
}

std::vector<FakeTrackingProvider::ScriptedFrame> threeFrameScript() {
    ExpressionParameters first{};
    first.mouthOpen = 0.4F;
    first.headYaw = 0.1F;

    ExpressionParameters third{};
    third.eyeOpenLeft = 0.9F;
    third.browUpRight = 0.2F;

    return {
        FakeTrackingProvider::ScriptedFrame{.parameters = first, .confidence = 0.8F, .faceFound = true},
        FakeTrackingProvider::ScriptedFrame{
            .parameters = ExpressionParameters{}, .confidence = 0.0F, .faceFound = false},
        FakeTrackingProvider::ScriptedFrame{.parameters = third, .confidence = 0.95F, .faceFound = true},
    };
}

TEST(FakeTrackingProviderTest, ProviderIdDefaultsToFakeTracker) {
    FakeTrackingProvider provider{threeFrameScript()};
    EXPECT_EQ(provider.providerId().value(), "fake-tracker");
}

TEST(FakeTrackingProviderTest, ProviderIdCanBeOverridden) {
    FakeTrackingProvider provider{threeFrameScript(), AvatarProviderId::create("custom-id").value()};
    EXPECT_EQ(provider.providerId().value(), "custom-id");
}

TEST(FakeTrackingProviderTest, EmitsScriptedParametersInOrder) {
    FakeTrackingProvider provider{threeFrameScript()};

    const auto first = provider.process(makeFrame(0));
    ASSERT_TRUE(first.hasValue());
    EXPECT_FLOAT_EQ(first.value().raw.mouthOpen, 0.4F);
    EXPECT_FLOAT_EQ(first.value().raw.headYaw, 0.1F);
    EXPECT_FLOAT_EQ(first.value().confidence, 0.8F);
    EXPECT_TRUE(first.value().faceFound);

    const auto second = provider.process(makeFrame(1'000));
    ASSERT_TRUE(second.hasValue());
    EXPECT_FALSE(second.value().faceFound);
    EXPECT_FLOAT_EQ(second.value().confidence, 0.0F);

    const auto third = provider.process(makeFrame(2'000));
    ASSERT_TRUE(third.hasValue());
    EXPECT_FLOAT_EQ(third.value().raw.eyeOpenLeft, 0.9F);
    EXPECT_FLOAT_EQ(third.value().raw.browUpRight, 0.2F);
    EXPECT_TRUE(third.value().faceFound);
}

TEST(FakeTrackingProviderTest, ClampsToFinalEntryAfterScriptIsExhausted) {
    FakeTrackingProvider provider{threeFrameScript()};
    ASSERT_TRUE(provider.process(makeFrame(0)).hasValue());
    ASSERT_TRUE(provider.process(makeFrame(1)).hasValue());
    ASSERT_TRUE(provider.process(makeFrame(2)).hasValue());

    const auto fourth = provider.process(makeFrame(3));
    const auto fifth = provider.process(makeFrame(4));

    ASSERT_TRUE(fourth.hasValue());
    ASSERT_TRUE(fifth.hasValue());
    // Both repeat the script's last entry (index 2), not a wraparound to
    // index 0 - a caller that overruns the script does not silently start
    // seeing an earlier, unrelated frame's data again.
    EXPECT_FLOAT_EQ(fourth.value().raw.eyeOpenLeft, 0.9F);
    EXPECT_FLOAT_EQ(fifth.value().raw.eyeOpenLeft, 0.9F);
}

TEST(FakeTrackingProviderTest, EmptyScriptFailsRatherThanReturningAPlausibleDefault) {
    FakeTrackingProvider provider{std::vector<FakeTrackingProvider::ScriptedFrame>{}};

    const auto result = provider.process(makeFrame(0));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeTrackingProviderTest, SameScriptAndFramesProduceIdenticalOutputsAcrossTwoRuns) {
    const std::vector<TimestampNs> timestamps{
        TimestampNs{DurationNs{0}},
        TimestampNs{DurationNs{16'666'667}},
        TimestampNs{DurationNs{33'333'334}},
    };

    auto runOnce = [&timestamps]() {
        FakeTrackingProvider provider{threeFrameScript()};
        std::vector<TrackingResult> results;
        for (const TimestampNs& ts : timestamps) {
            creator::media::VideoFrame frame{};
            frame.timestamp = ts;
            const auto processed = provider.process(frame);
            results.push_back(processed.value());
        }
        return results;
    };

    const std::vector<TrackingResult> runA = runOnce();
    const std::vector<TrackingResult> runB = runOnce();

    ASSERT_EQ(runA.size(), runB.size());
    for (std::size_t i = 0; i < runA.size(); ++i) {
        EXPECT_EQ(runA[i].timestamp, runB[i].timestamp) << "index " << i;
        EXPECT_EQ(runA[i].raw, runB[i].raw) << "index " << i;
        EXPECT_FLOAT_EQ(runA[i].confidence, runB[i].confidence) << "index " << i;
        EXPECT_EQ(runA[i].faceFound, runB[i].faceFound) << "index " << i;
    }
}

TEST(FakeTrackingProviderTest, TimestampComesFromTheFrameNotTheScript) {
    FakeTrackingProvider provider{threeFrameScript()};

    const auto result = provider.process(makeFrame(123'456'789));

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().timestamp, TimestampNs{DurationNs{123'456'789}});
}

// The pixel-ignoring guarantee: feed a frame produced by FakeCaptureSource,
// whose platformHandle is always null (it exists only to prove timing - see
// FakeCaptureSource.h). If FakeTrackingProvider ever dereferenced
// platformHandle it would crash here; instead it must still return the
// scripted result untouched, proving process() never looked at pixels at
// all.
TEST(FakeTrackingProviderTest, IgnoresPixelsFromARealPixelLessCaptureFrame) {
    FakeCaptureSource captureSource{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(captureSource.start(creator::capture::CaptureConfig{}).hasValue());
    const auto frame = captureSource.tick();
    ASSERT_TRUE(frame.hasValue());
    ASSERT_EQ(frame.value().platformHandle, nullptr);

    FakeTrackingProvider provider{threeFrameScript()};
    const auto result = provider.process(frame.value());

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().timestamp, frame.value().timestamp);
    EXPECT_FLOAT_EQ(result.value().raw.mouthOpen, 0.4F);
    EXPECT_TRUE(result.value().faceFound);
}

}  // namespace
