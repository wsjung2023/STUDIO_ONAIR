#include "fakes/FakeCaptureSource.h"

#include "capture/ICaptureSource.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using creator::capture::CaptureConfig;
using creator::capture::ICaptureSource;
using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::fakes::FakeCaptureSource;

FakeCaptureSource makeSource() {
    return FakeCaptureSource{SourceId::create("screen-1").value(), "Fake Screen"};
}

/// Mirrors frameToTimestamp's own ceiling-division formula (core/Timebase.cpp)
/// without calling it, so this test independently pins the "frame N lands at
/// N/rate seconds, rounded up" contract FakeCaptureSource must honour rather
/// than merely echoing whatever the shared function currently does.
///
/// A plain `frame * 1'000'000'000LL / rateNumerator` truncates instead of
/// rounding up, and disagrees with frameToTimestamp for every frame index that
/// is not an exact multiple of the rate - e.g. frame 1 at 60fps: truncating
/// gives 16'666'666, but frameToTimestamp rounds up to 16'666'667. Verified by
/// simulating both formulas for every frame in range: they differ for 40 of
/// the 61 frames this test walks.
std::int64_t expectedNs(std::int64_t frameIndex, std::int64_t rateNumerator) {
    return (frameIndex * 1'000'000'000LL + (rateNumerator - 1)) / rateNumerator;
}

TEST(FakeCaptureSourceTest, ReportsIdentity) {
    const FakeCaptureSource source = makeSource();

    EXPECT_EQ(source.id().value(), "screen-1");
    EXPECT_EQ(source.displayName(), "Fake Screen");
}

TEST(FakeCaptureSourceTest, StartsAndStops) {
    FakeCaptureSource source = makeSource();

    EXPECT_TRUE(source.start(CaptureConfig{}).hasValue());
    EXPECT_TRUE(source.stop().hasValue());
}

TEST(FakeCaptureSourceTest, ProducesSixtyFpsTimestamps) {
    FakeCaptureSource source = makeSource();
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    // Default config is 60/1, so frames land exactly 1/60s apart and frame 60
    // is exactly one second in. No clock is read: the timestamp is computed
    // from the frame index.
    for (std::int64_t frame = 0; frame <= 60; ++frame) {
        const auto produced = source.tick();
        ASSERT_TRUE(produced.hasValue()) << "frame " << frame;
        EXPECT_EQ(produced.value().timestamp.time_since_epoch().count(), expectedNs(frame, 60))
            << "frame " << frame;
    }
}

TEST(FakeCaptureSourceTest, HonoursConfiguredFrameRate) {
    FakeCaptureSource source = makeSource();
    CaptureConfig config;
    config.frameRateNumerator = 30;
    config.frameRateDenominator = 1;
    ASSERT_TRUE(source.start(config).hasValue());

    ASSERT_TRUE(source.tick().hasValue());
    const auto second = source.tick();

    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(second.value().timestamp.time_since_epoch().count(), expectedNs(1, 30));
}

TEST(FakeCaptureSourceTest, FramesCarryConfiguredGeometry) {
    FakeCaptureSource source = makeSource();
    CaptureConfig config;
    config.targetWidth = 1280;
    config.targetHeight = 720;
    ASSERT_TRUE(source.start(config).hasValue());

    const auto produced = source.tick();

    ASSERT_TRUE(produced.hasValue());
    EXPECT_EQ(produced.value().width, 1280u);
    EXPECT_EQ(produced.value().height, 720u);
    EXPECT_EQ(produced.value().colorSpace, creator::media::ColorSpace::Rec709Sdr);
}

TEST(FakeCaptureSourceTest, CountsReceivedFrames) {
    FakeCaptureSource source = makeSource();
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(source.tick().hasValue());
    }

    EXPECT_EQ(source.stats().receivedFrames, 5u);
    EXPECT_EQ(source.stats().droppedFrames, 0u);
    EXPECT_DOUBLE_EQ(source.stats().currentFps, 60.0);
}

TEST(FakeCaptureSourceTest, RejectsDoubleStart) {
    FakeCaptureSource source = makeSource();
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    const auto result = source.start(CaptureConfig{});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeCaptureSourceTest, RejectsTickBeforeStart) {
    FakeCaptureSource source = makeSource();

    const auto result = source.tick();

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeCaptureSourceTest, RejectsTickAfterStop) {
    FakeCaptureSource source = makeSource();
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());
    ASSERT_TRUE(source.stop().hasValue());

    const auto result = source.tick();

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeCaptureSourceTest, RejectsInvalidFrameRate) {
    FakeCaptureSource source = makeSource();
    CaptureConfig config;
    config.frameRateDenominator = 0;

    const auto result = source.start(config);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(FakeCaptureSourceTest, StopIsIdempotent) {
    // Real sources must survive stop() on a source that never started - a
    // failed start still has to be cleaned up. The fake holds that contract.
    FakeCaptureSource source = makeSource();

    EXPECT_TRUE(source.stop().hasValue());
    EXPECT_TRUE(source.stop().hasValue());
}

TEST(FakeCaptureSourceTest, RestartResetsTheTimeline) {
    FakeCaptureSource source = makeSource();
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());
    ASSERT_TRUE(source.tick().hasValue());
    ASSERT_TRUE(source.tick().hasValue());
    ASSERT_TRUE(source.stop().hasValue());

    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());
    const auto produced = source.tick();

    ASSERT_TRUE(produced.hasValue());
    EXPECT_EQ(produced.value().timestamp.time_since_epoch().count(), 0);
    EXPECT_EQ(source.stats().receivedFrames, 1u);
}

TEST(FakeCaptureSourceTest, IsUsableThroughThePortInterface) {
    // The point of the fake: code above the port must not know it is fake.
    std::unique_ptr<ICaptureSource> source =
        std::make_unique<FakeCaptureSource>(SourceId::create("screen-1").value(), "Fake Screen");

    EXPECT_TRUE(source->start(CaptureConfig{}).hasValue());
    EXPECT_EQ(source->id().value(), "screen-1");
    EXPECT_TRUE(source->stop().hasValue());
}

}  // namespace
