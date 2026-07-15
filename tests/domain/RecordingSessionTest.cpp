#include "domain/RecordingSession.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/Segment.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::domain::RecordingSession;
using creator::domain::SegmentInfo;
using creator::domain::SegmentStatus;
using creator::domain::SessionId;
using creator::domain::SessionState;
using creator::domain::SourceId;

TimestampNs at(std::int64_t seconds) {
    return TimestampNs{std::chrono::duration_cast<DurationNs>(std::chrono::seconds{seconds})};
}

RecordingSession makeSession() {
    return RecordingSession{SessionId::create("session-1").value()};
}

SegmentInfo makeSegment(std::uint64_t index) {
    return SegmentInfo{
        .index = index,
        .sourceId = SourceId::create("screen-1").value(),
        .startTime = at(static_cast<std::int64_t>(index) * 2),
        .duration = std::chrono::seconds{2},
        .status = SegmentStatus::Ready,
        .relativePath = "media/screen/segment_000001.mkv",
    };
}

TEST(RecordingSessionTest, StartsIdle) {
    const RecordingSession session = makeSession();

    EXPECT_EQ(session.state(), SessionState::Idle);
    EXPECT_EQ(session.segmentCount(), 0u);
    EXPECT_EQ(session.id().value(), "session-1");
}

TEST(RecordingSessionTest, StartMovesToRecording) {
    RecordingSession session = makeSession();

    ASSERT_TRUE(session.start(at(0)).hasValue());
    EXPECT_EQ(session.state(), SessionState::Recording);
}

TEST(RecordingSessionTest, StopMovesToStopped) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());

    ASSERT_TRUE(session.stop(at(10)).hasValue());
    EXPECT_EQ(session.state(), SessionState::Stopped);
}

TEST(RecordingSessionTest, ReportsDurationAfterStop) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());
    ASSERT_TRUE(session.stop(at(10)).hasValue());

    EXPECT_EQ(session.duration(), DurationNs{std::chrono::seconds{10}});
}

TEST(RecordingSessionTest, DurationIsZeroBeforeStop) {
    RecordingSession session = makeSession();
    EXPECT_EQ(session.duration(), DurationNs::zero());

    ASSERT_TRUE(session.start(at(0)).hasValue());
    EXPECT_EQ(session.duration(), DurationNs::zero());
}

TEST(RecordingSessionTest, CollectsSegmentsWhileRecording) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());

    ASSERT_TRUE(session.addSegment(makeSegment(0)).hasValue());
    ASSERT_TRUE(session.addSegment(makeSegment(1)).hasValue());

    EXPECT_EQ(session.segmentCount(), 2u);
    EXPECT_EQ(session.segments().at(1).index, 1u);
    EXPECT_EQ(session.segments().at(1).sourceId.value(), "screen-1");
}

TEST(RecordingSessionTest, KeepsSegmentsAfterStop) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());
    ASSERT_TRUE(session.addSegment(makeSegment(0)).hasValue());
    ASSERT_TRUE(session.stop(at(2)).hasValue());

    EXPECT_EQ(session.segmentCount(), 1u);
}

TEST(RecordingSessionTest, RejectsDoubleStart) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());

    const auto result = session.start(at(1));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(session.state(), SessionState::Recording);
}

TEST(RecordingSessionTest, RejectsStopWithoutStart) {
    RecordingSession session = makeSession();

    const auto result = session.stop(at(1));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(session.state(), SessionState::Idle);
}

TEST(RecordingSessionTest, RejectsDoubleStop) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());
    ASSERT_TRUE(session.stop(at(2)).hasValue());

    const auto result = session.stop(at(3));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(RecordingSessionTest, RejectsRestartAfterStop) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());
    ASSERT_TRUE(session.stop(at(2)).hasValue());

    const auto result = session.start(at(3));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(RecordingSessionTest, RejectsSegmentBeforeStart) {
    RecordingSession session = makeSession();

    const auto result = session.addSegment(makeSegment(0));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(session.segmentCount(), 0u);
}

TEST(RecordingSessionTest, RejectsSegmentAfterStop) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(0)).hasValue());
    ASSERT_TRUE(session.stop(at(2)).hasValue());

    const auto result = session.addSegment(makeSegment(0));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(RecordingSessionTest, RejectsStopBeforeStartTime) {
    RecordingSession session = makeSession();
    ASSERT_TRUE(session.start(at(10)).hasValue());

    const auto result = session.stop(at(9));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(session.state(), SessionState::Recording);
}

}  // namespace
