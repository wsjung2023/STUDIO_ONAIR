#include "domain/DeleteRangeCommand.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::DeleteRangeCommand;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

ClipId clipId(std::string value) {
    return ClipId::create(std::move(value)).value();
}

TrackId trackId(std::string value) {
    return TrackId::create(std::move(value)).value();
}

MediaAsset asset() {
    return MediaAsset::create(
               AssetId::create("video-asset").value(), MediaKind::Video,
               "media/video.mkv", DurationNs{1000},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 1000, "hash", AssetAvailability::Available)
        .value();
}

Clip clip(std::string id, std::int64_t start, std::int64_t duration,
          std::int64_t sourceStart = -1) {
    if (sourceStart < 0) sourceStart = start;
    return Clip::createAsset(
               clipId(std::move(id)), asset(),
               TimeRange::create(at(sourceStart), DurationNs{duration}).value(),
               TimeRange::create(at(start), DurationNs{duration}).value(), true,
               std::nullopt, std::nullopt)
        .value();
}

Timeline emptyTimeline() {
    return Timeline::create(TimelineId::create("timeline").value(), "Main",
                            FrameRate::create(60, 1).value())
        .value();
}

void addTrack(Timeline& timeline, std::string id) {
    ASSERT_TRUE(timeline.addTrack(
                            Track::create(trackId(id), TrackKind::Video, id, true, false)
                                .value())
                    .hasValue());
}

void addClip(Timeline& timeline, std::string track, Clip value) {
    ASSERT_TRUE(timeline.insertClip(trackId(std::move(track)), std::move(value)).hasValue());
}

DeleteRangeCommand command(std::int64_t start, std::int64_t duration, bool ripple,
                           std::vector<ClipId> rightIds = {}) {
    return DeleteRangeCommand{
        CommandId::create("delete").value(),
        TimeRange::create(at(start), DurationNs{duration}).value(), ripple,
        std::move(rightIds)};
}

TEST(DeleteRangeCommandTest, NonRippleRemovesContainedClipsWithoutMovingOthers) {
    auto timeline = emptyTimeline();
    addTrack(timeline, "v1");
    addClip(timeline, "v1", clip("before", 0, 10));
    addClip(timeline, "v1", clip("inside", 20, 10));
    addClip(timeline, "v1", clip("after", 40, 10));
    auto edit = command(15, 20, false);

    ASSERT_TRUE(edit.execute(timeline).hasValue());

    const auto& clips = timeline.track(trackId("v1"))->clips();
    ASSERT_EQ(clips.size(), 2U);
    EXPECT_EQ(clips[0].id(), clipId("before"));
    EXPECT_EQ(clips[1].id(), clipId("after"));
    EXPECT_EQ(clips[1].timelineRange().start(), at(40));
}

TEST(DeleteRangeCommandTest, TrimsLeftAndRightOverlapAtExactBoundaries) {
    auto left = emptyTimeline();
    addTrack(left, "v1");
    addClip(left, "v1", clip("left", 0, 20));
    auto leftEdit = command(10, 10, false);
    ASSERT_TRUE(leftEdit.execute(left).hasValue());
    ASSERT_EQ(left.track(trackId("v1"))->clips().size(), 1U);
    EXPECT_EQ(left.track(trackId("v1"))->clips()[0].timelineRange(),
              TimeRange::create(at(0), DurationNs{10}).value());
    EXPECT_EQ(left.track(trackId("v1"))->clips()[0].sourceRange(),
              TimeRange::create(at(0), DurationNs{10}).value());

    auto right = emptyTimeline();
    addTrack(right, "v1");
    addClip(right, "v1", clip("right", 10, 20, 100));
    auto rightEdit = command(5, 10, false);
    ASSERT_TRUE(rightEdit.execute(right).hasValue());
    ASSERT_EQ(right.track(trackId("v1"))->clips().size(), 1U);
    EXPECT_EQ(right.track(trackId("v1"))->clips()[0].timelineRange(),
              TimeRange::create(at(15), DurationNs{15}).value());
    EXPECT_EQ(right.track(trackId("v1"))->clips()[0].sourceRange(),
              TimeRange::create(at(105), DurationNs{15}).value());
}

TEST(DeleteRangeCommandTest, SplitsSpanningClipWithCallerSuppliedIdentity) {
    auto timeline = emptyTimeline();
    addTrack(timeline, "v1");
    addClip(timeline, "v1", clip("whole", 0, 30, 100));
    auto edit = command(10, 10, false, {clipId("right")});

    ASSERT_TRUE(edit.execute(timeline).hasValue());

    const auto& clips = timeline.track(trackId("v1"))->clips();
    ASSERT_EQ(clips.size(), 2U);
    EXPECT_EQ(clips[0].id(), clipId("whole"));
    EXPECT_EQ(clips[0].timelineRange(),
              TimeRange::create(at(0), DurationNs{10}).value());
    EXPECT_EQ(clips[1].id(), clipId("right"));
    EXPECT_EQ(clips[1].timelineRange(),
              TimeRange::create(at(20), DurationNs{10}).value());
    EXPECT_EQ(clips[1].sourceRange(),
              TimeRange::create(at(120), DurationNs{10}).value());
}

TEST(DeleteRangeCommandTest, RippleClosesDeletedRangeForSplitAndFollowingClips) {
    auto timeline = emptyTimeline();
    addTrack(timeline, "v1");
    addClip(timeline, "v1", clip("whole", 0, 30, 100));
    addClip(timeline, "v1", clip("after", 40, 10, 200));
    auto edit = command(10, 10, true, {clipId("right")});

    ASSERT_TRUE(edit.execute(timeline).hasValue());

    const auto& clips = timeline.track(trackId("v1"))->clips();
    ASSERT_EQ(clips.size(), 3U);
    EXPECT_EQ(clips[1].timelineRange(),
              TimeRange::create(at(10), DurationNs{10}).value());
    EXPECT_EQ(clips[2].timelineRange(),
              TimeRange::create(at(30), DurationNs{10}).value());
    EXPECT_EQ(clips[2].sourceRange(),
              TimeRange::create(at(200), DurationNs{10}).value());
}

TEST(DeleteRangeCommandTest, RippleClosesEmptyGapAcrossUnlockedTracks) {
    auto timeline = emptyTimeline();
    addTrack(timeline, "screen");
    addTrack(timeline, "camera");
    addClip(timeline, "screen", clip("screen", 30, 10));
    addClip(timeline, "camera", clip("camera", 40, 10));
    auto edit = command(10, 10, true);

    ASSERT_TRUE(edit.execute(timeline).hasValue());

    EXPECT_EQ(timeline.track(trackId("screen"))->clips()[0].timelineRange().start(), at(20));
    EXPECT_EQ(timeline.track(trackId("camera"))->clips()[0].timelineRange().start(), at(30));
}

TEST(DeleteRangeCommandTest, PreservesLockedTrackAndEditsUnlockedTrack) {
    auto timeline = emptyTimeline();
    addTrack(timeline, "locked");
    addTrack(timeline, "open");
    addClip(timeline, "locked", clip("locked-clip", 30, 10));
    addClip(timeline, "open", clip("open-clip", 30, 10));
    ASSERT_TRUE(timeline.setTrackLocked(trackId("locked"), true).hasValue());
    const auto lockedBefore = *timeline.track(trackId("locked"));
    auto edit = command(10, 10, true);

    ASSERT_TRUE(edit.execute(timeline).hasValue());

    EXPECT_EQ(*timeline.track(trackId("locked")), lockedBefore);
    EXPECT_EQ(timeline.track(trackId("open"))->clips()[0].timelineRange().start(), at(20));
}

TEST(DeleteRangeCommandTest, UndoRestoresEveryAffectedTrackExactly) {
    auto timeline = emptyTimeline();
    addTrack(timeline, "v1");
    addTrack(timeline, "v2");
    addClip(timeline, "v1", clip("span", 0, 30));
    addClip(timeline, "v2", clip("after", 40, 10));
    const auto before = timeline;
    auto edit = command(10, 10, true, {clipId("span-right")});
    ASSERT_TRUE(edit.execute(timeline).hasValue());

    ASSERT_TRUE(edit.undo(timeline).hasValue());

    EXPECT_EQ(timeline, before);
}

TEST(DeleteRangeCommandTest, MissingOrDuplicateSplitIdentityFailsAtomically) {
    auto missingTimeline = emptyTimeline();
    addTrack(missingTimeline, "v1");
    addClip(missingTimeline, "v1", clip("span", 0, 30));
    const auto missingBefore = missingTimeline;
    auto missing = command(10, 10, false);
    const auto missingResult = missing.execute(missingTimeline);
    ASSERT_FALSE(missingResult.hasValue());
    EXPECT_EQ(missingResult.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(missingTimeline, missingBefore);

    auto duplicateTimeline = emptyTimeline();
    addTrack(duplicateTimeline, "v1");
    addClip(duplicateTimeline, "v1", clip("span", 0, 30));
    addClip(duplicateTimeline, "v1", clip("existing", 40, 10));
    const auto duplicateBefore = duplicateTimeline;
    auto duplicate = command(10, 10, false, {clipId("existing")});
    const auto duplicateResult = duplicate.execute(duplicateTimeline);
    ASSERT_FALSE(duplicateResult.hasValue());
    EXPECT_EQ(duplicateResult.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(duplicateTimeline, duplicateBefore);
}

}  // namespace
