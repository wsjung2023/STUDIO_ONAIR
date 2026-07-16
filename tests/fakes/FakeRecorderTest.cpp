#include "fakes/FakeRecorder.h"

#include "domain/Identifiers.h"
#include "domain/RecordingSession.h"
#include "fakes/FakeCaptureSource.h"
#include "recorder/IRecorder.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using creator::capture::CaptureConfig;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::domain::SegmentStatus;
using creator::domain::SessionId;
using creator::domain::SessionState;
using creator::domain::SourceId;
using creator::fakes::FakeCaptureSource;
using creator::fakes::FakeRecorder;
using creator::recorder::RecorderConfig;

TimestampNs at(std::int64_t seconds) {
    return TimestampNs{std::chrono::duration_cast<DurationNs>(std::chrono::seconds{seconds})};
}

RecorderConfig makeConfig() {
    return RecorderConfig{
        .sessionId = SessionId::create("session-1").value(),
        .sourceId = SourceId::create("screen-1").value(),
        .segmentDuration = std::chrono::seconds{2},
    };
}

TEST(FakeRecorderTest, StartsAndStops) {
    FakeRecorder recorder;

    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());
    const auto session = recorder.stop(at(4));

    ASSERT_TRUE(session.hasValue());
    EXPECT_EQ(session.value().state(), SessionState::Stopped);
    EXPECT_EQ(session.value().id().value(), "session-1");
    EXPECT_EQ(session.value().duration(), DurationNs{std::chrono::seconds{4}});
}

TEST(FakeRecorderTest, ClosesASegmentEverySegmentDuration) {
    FakeRecorder recorder;
    FakeCaptureSource source{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    // 6 seconds of 60fps frames at a 2 second segment length = 3 segments.
    for (int frame = 0; frame < 60 * 6; ++frame) {
        const auto produced = source.tick();
        ASSERT_TRUE(produced.hasValue());
        ASSERT_TRUE(recorder.accept(produced.value()).hasValue());
    }
    const auto session = recorder.stop(at(6));

    ASSERT_TRUE(session.hasValue());
    EXPECT_EQ(session.value().segmentCount(), 3u);
    EXPECT_EQ(recorder.stats().framesAccepted, 360u);
    EXPECT_EQ(recorder.stats().segmentsWritten, 3u);
}

TEST(FakeRecorderTest, SegmentsCarryIndexSourceAndPath) {
    FakeRecorder recorder;
    FakeCaptureSource source{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    for (int frame = 0; frame < 60 * 4; ++frame) {
        const auto produced = source.tick();
        ASSERT_TRUE(produced.hasValue());
        ASSERT_TRUE(recorder.accept(produced.value()).hasValue());
    }
    const auto session = recorder.stop(at(4));

    ASSERT_TRUE(session.hasValue());
    ASSERT_EQ(session.value().segmentCount(), 2u);

    const auto& first = session.value().segments().at(0);
    EXPECT_EQ(first.index, 0u);
    EXPECT_EQ(first.sourceId.value(), "screen-1");
    EXPECT_EQ(first.startTime.time_since_epoch().count(), 0);
    EXPECT_EQ(first.duration, DurationNs{std::chrono::seconds{2}});
    EXPECT_EQ(first.status, SegmentStatus::Ready);
    EXPECT_EQ(first.relativePath, "media/screen-1/segment_000000.mkv");

    const auto& second = session.value().segments().at(1);
    EXPECT_EQ(second.index, 1u);
    EXPECT_EQ(second.startTime, at(2));
    EXPECT_EQ(second.relativePath, "media/screen-1/segment_000001.mkv");
}

TEST(FakeRecorderTest, FlushesTrailingPartialSegmentOnStop) {
    FakeRecorder recorder;
    FakeCaptureSource source{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    // 3 seconds: one full 2s segment plus a 1s remainder. Dropping the
    // remainder would silently lose the last second of every take.
    for (int frame = 0; frame < 60 * 3; ++frame) {
        const auto produced = source.tick();
        ASSERT_TRUE(produced.hasValue());
        ASSERT_TRUE(recorder.accept(produced.value()).hasValue());
    }
    const auto session = recorder.stop(at(3));

    ASSERT_TRUE(session.hasValue());
    ASSERT_EQ(session.value().segmentCount(), 2u);
    EXPECT_EQ(session.value().segments().at(1).status, SegmentStatus::Ready);
    EXPECT_LT(session.value().segments().at(1).duration, DurationNs{std::chrono::seconds{2}});
}

TEST(FakeRecorderTest, ProducesNoSegmentsWithoutFrames) {
    FakeRecorder recorder;
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());

    const auto session = recorder.stop(at(4));

    ASSERT_TRUE(session.hasValue());
    EXPECT_EQ(session.value().segmentCount(), 0u);
}

TEST(FakeRecorderTest, RejectsDoubleStart) {
    FakeRecorder recorder;
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());

    const auto result = recorder.start(makeConfig(), at(1));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeRecorderTest, RejectsStopWithoutStart) {
    FakeRecorder recorder;

    const auto result = recorder.stop(at(1));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeRecorderTest, RejectsFrameBeforeStart) {
    FakeRecorder recorder;
    FakeCaptureSource source{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());
    const auto produced = source.tick();
    ASSERT_TRUE(produced.hasValue());

    const auto result = recorder.accept(produced.value());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(FakeRecorderTest, RejectsInvalidSegmentDuration) {
    FakeRecorder recorder;
    RecorderConfig config = makeConfig();
    config.segmentDuration = DurationNs::zero();

    const auto result = recorder.start(config, at(0));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(FakeRecorderTest, ClosesSegmentForFrameLandingExactlyOnBoundary) {
    FakeRecorder recorder;
    FakeCaptureSource source{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    // 121 frames at 60fps = indices 0..120. Frame 120's timestamp is exactly
    // 2s - the start of the second segment, not the end of the first
    // (frameToTimestamp(120, 60/1) == 2'000'000'000ns exactly, no rounding).
    // That frame must count as "seen" in the new segment, or stop()'s tail
    // flush finds nothing to close and the take silently loses it.
    for (int frame = 0; frame < 121; ++frame) {
        const auto produced = source.tick();
        ASSERT_TRUE(produced.hasValue());
        ASSERT_TRUE(recorder.accept(produced.value()).hasValue());
    }
    const auto session = recorder.stop(at(2));

    ASSERT_TRUE(session.hasValue());
    // Bug: this reads 1 - the boundary frame closes segment 0 but then
    // forgets it ever arrived, so the trailing (zero-length) segment 1 that
    // frame 120 belongs to never gets flushed.
    EXPECT_EQ(session.value().segmentCount(), 2u);
    EXPECT_EQ(recorder.stats().segmentsWritten, 2u);
}

TEST(FakeRecorderTest, CanRecordASecondTake) {
    FakeRecorder recorder;
    FakeCaptureSource source{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(recorder.start(makeConfig(), at(0)).hasValue());
    ASSERT_TRUE(source.start(CaptureConfig{}).hasValue());

    // Feed the first take, or the counter assertion below cannot tell "reset on
    // start" from "no reset logic at all" - both leave it at zero.
    for (int frame = 0; frame < 30; ++frame) {
        const auto produced = source.tick();
        ASSERT_TRUE(produced.hasValue());
        ASSERT_TRUE(recorder.accept(produced.value()).hasValue());
    }
    ASSERT_EQ(recorder.stats().framesAccepted, 30u);
    ASSERT_TRUE(recorder.stop(at(2)).hasValue());

    RecorderConfig second = makeConfig();
    second.sessionId = SessionId::create("session-2").value();
    ASSERT_TRUE(recorder.start(second, at(10)).hasValue());
    const auto session = recorder.stop(at(12));

    ASSERT_TRUE(session.hasValue());
    EXPECT_EQ(session.value().id().value(), "session-2");
    EXPECT_EQ(session.value().duration(), DurationNs{std::chrono::seconds{2}});
    // Counters are per-take, not cumulative. This is only a real check because
    // the first take pushed framesAccepted to 30 above.
    EXPECT_EQ(recorder.stats().framesAccepted, 0u);
}

}  // namespace
