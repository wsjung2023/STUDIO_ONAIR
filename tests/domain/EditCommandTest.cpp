#include "domain/EditHistory.h"
#include "domain/SplitClipCommand.h"
#include "domain/TrimClipCommand.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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
using creator::domain::EditHistory;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::SplitClipCommand;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::TrimClipCommand;
using creator::domain::TrimEdge;
using creator::domain::VideoAssetMetadata;

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{DurationNs{nanoseconds}};
}

ClipId clipId(std::string value) {
    return ClipId::create(std::move(value)).value();
}

CommandId commandId(std::string value) {
    return CommandId::create(std::move(value)).value();
}

TrackId videoTrackId() {
    return TrackId::create("v1").value();
}

MediaAsset asset() {
    return MediaAsset::create(
               AssetId::create("asset").value(), MediaKind::Video, "media/source.mkv",
               DurationNs{100},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 1000, "hash", AssetAvailability::Available)
        .value();
}

Timeline timeline() {
    auto value = Timeline::create(TimelineId::create("timeline").value(), "Main",
                                  FrameRate::create(60, 1).value())
                     .value();
    EXPECT_TRUE(value.addTrack(
                         Track::create(videoTrackId(), TrackKind::Video, "Screen", true, false)
                             .value())
                    .hasValue());
    auto created = Clip::createAsset(
        clipId("original"), asset(),
        TimeRange::create(at(0), DurationNs{100}).value(),
        TimeRange::create(at(0), DurationNs{100}).value(), true,
        std::nullopt, std::nullopt);
    EXPECT_TRUE(created.hasValue())
        << (created.hasValue() ? std::string{} : created.error().message());
    if (created.hasValue()) {
        EXPECT_TRUE(value.insertClip(videoTrackId(), std::move(created).value()).hasValue());
    }
    return value;
}

const Clip& onlyClip(const Timeline& value) {
    return value.track(videoTrackId())->clips().front();
}

TEST(EditCommandTest, SplitCreatesExactSourceAndTimelineRanges) {
    auto value = timeline();
    SplitClipCommand command{commandId("split"), videoTrackId(), clipId("original"),
                             clipId("right"), at(40)};

    ASSERT_TRUE(command.execute(value).hasValue());

    const auto& clips = value.track(videoTrackId())->clips();
    ASSERT_EQ(clips.size(), 2U);
    EXPECT_EQ(clips[0].id(), clipId("original"));
    EXPECT_EQ(clips[0].sourceRange(),
              TimeRange::create(at(0), DurationNs{40}).value());
    EXPECT_EQ(clips[0].timelineRange(),
              TimeRange::create(at(0), DurationNs{40}).value());
    EXPECT_EQ(clips[1].id(), clipId("right"));
    EXPECT_EQ(clips[1].sourceRange(),
              TimeRange::create(at(40), DurationNs{60}).value());
    EXPECT_EQ(clips[1].timelineRange(),
              TimeRange::create(at(40), DurationNs{60}).value());
    EXPECT_EQ(command.record().type, "SPLIT_CLIP");
    EXPECT_NE(command.record().payload.find("\"splitNs\":40"), std::string::npos);
}

TEST(EditCommandTest, SplitUndoRestoresOriginalExactly) {
    auto value = timeline();
    const auto before = value;
    SplitClipCommand command{commandId("split"), videoTrackId(), clipId("original"),
                             clipId("right"), at(40)};
    ASSERT_TRUE(command.execute(value).hasValue());

    ASSERT_TRUE(command.undo(value).hasValue());

    EXPECT_EQ(value, before);
}

TEST(EditCommandTest, SplitRejectsBoundaryWithoutMutation) {
    auto value = timeline();
    const auto before = value;
    SplitClipCommand atStart{commandId("split"), videoTrackId(), clipId("original"),
                             clipId("right"), at(0)};

    const auto result = atStart.execute(value);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(value, before);
}

TEST(EditCommandTest, LeadingAndTrailingTrimMoveCorrectSourceEdges) {
    auto leadingTimeline = timeline();
    TrimClipCommand leading{commandId("leading"), videoTrackId(), clipId("original"),
                            TrimEdge::Leading, at(10)};
    ASSERT_TRUE(leading.execute(leadingTimeline).hasValue());
    EXPECT_EQ(onlyClip(leadingTimeline).sourceRange(),
              TimeRange::create(at(10), DurationNs{90}).value());
    EXPECT_EQ(onlyClip(leadingTimeline).timelineRange(),
              TimeRange::create(at(10), DurationNs{90}).value());

    auto trailingTimeline = timeline();
    const auto before = trailingTimeline;
    TrimClipCommand trailing{commandId("trailing"), videoTrackId(), clipId("original"),
                             TrimEdge::Trailing, at(80)};
    ASSERT_TRUE(trailing.execute(trailingTimeline).hasValue());
    EXPECT_EQ(onlyClip(trailingTimeline).sourceRange(),
              TimeRange::create(at(0), DurationNs{80}).value());
    EXPECT_EQ(onlyClip(trailingTimeline).timelineRange(),
              TimeRange::create(at(0), DurationNs{80}).value());
    ASSERT_TRUE(trailing.undo(trailingTimeline).hasValue());
    EXPECT_EQ(trailingTimeline, before);
}

TEST(EditCommandTest, CommandPayloadEscapesControlCharactersAsJson) {
    TrimClipCommand command{
        commandId("trim"), videoTrackId(), clipId("clip\nidentifier"),
        TrimEdge::Trailing, at(80)};

    const auto record = command.record();

    EXPECT_EQ(record.payload.find('\n'), std::string::npos);
    EXPECT_NE(record.payload.find("clip\\nidentifier"), std::string::npos);
}

TEST(EditHistoryTest, FailedCommandDoesNotEnterHistory) {
    auto value = timeline();
    EditHistory history{10};

    const auto result = history.execute(
        value, std::make_unique<SplitClipCommand>(
                   commandId("bad"), videoTrackId(), clipId("original"),
                   clipId("right"), at(0)));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(history.size(), 0U);
    EXPECT_EQ(history.cursor(), 0U);
}

TEST(EditHistoryTest, UndoRedoAndBranchingUseOneCommandCursor) {
    auto value = timeline();
    EditHistory history{10};
    ASSERT_TRUE(history.execute(
                           value, std::make_unique<SplitClipCommand>(
                                      commandId("first"), videoTrackId(), clipId("original"),
                                      clipId("right-40"), at(40)))
                    .hasValue());
    ASSERT_TRUE(history.undo(value).hasValue());
    EXPECT_EQ(value, timeline());
    ASSERT_TRUE(history.redo(value).hasValue());
    EXPECT_EQ(value.track(videoTrackId())->clips().size(), 2U);
    ASSERT_TRUE(history.undo(value).hasValue());

    ASSERT_TRUE(history.execute(
                           value, std::make_unique<SplitClipCommand>(
                                      commandId("branch"), videoTrackId(), clipId("original"),
                                      clipId("right-60"), at(60)))
                    .hasValue());

    EXPECT_EQ(history.size(), 1U);
    EXPECT_EQ(history.cursor(), 1U);
    const auto redo = history.redo(value);
    ASSERT_FALSE(redo.hasValue());
    EXPECT_EQ(redo.error().code(), ErrorCode::InvalidState);
}

TEST(EditHistoryTest, BoundEvictsOnlyOldestCommittedCommand) {
    auto value = timeline();
    EditHistory history{2};
    ASSERT_TRUE(history.execute(
                           value, std::make_unique<TrimClipCommand>(
                                      commandId("one"), videoTrackId(), clipId("original"),
                                      TrimEdge::Trailing, at(90)))
                    .hasValue());
    ASSERT_TRUE(history.execute(
                           value, std::make_unique<TrimClipCommand>(
                                      commandId("two"), videoTrackId(), clipId("original"),
                                      TrimEdge::Leading, at(10)))
                    .hasValue());
    ASSERT_TRUE(history.execute(
                           value, std::make_unique<TrimClipCommand>(
                                      commandId("three"), videoTrackId(), clipId("original"),
                                      TrimEdge::Trailing, at(80)))
                    .hasValue());

    EXPECT_EQ(history.size(), 2U);
    EXPECT_EQ(history.cursor(), 2U);
    ASSERT_TRUE(history.undo(value).hasValue());
    ASSERT_TRUE(history.undo(value).hasValue());
    EXPECT_EQ(onlyClip(value).timelineRange(),
              TimeRange::create(at(0), DurationNs{90}).value());
    EXPECT_FALSE(history.undo(value).hasValue());
}

TEST(EditHistoryTest, CleanCheckpointAndCopyAreIndependent) {
    auto value = timeline();
    EditHistory history{10};
    EXPECT_TRUE(history.isClean());
    ASSERT_TRUE(history.execute(
                           value, std::make_unique<TrimClipCommand>(
                                      commandId("trim"), videoTrackId(), clipId("original"),
                                      TrimEdge::Trailing, at(90)))
                    .hasValue());
    EXPECT_FALSE(history.isClean());
    history.markClean();
    EXPECT_TRUE(history.isClean());

    auto copied = history;
    auto copiedTimeline = value;
    ASSERT_TRUE(copied.undo(copiedTimeline).hasValue());
    EXPECT_EQ(copied.cursor(), 0U);
    EXPECT_EQ(history.cursor(), 1U);
    EXPECT_NE(copiedTimeline, value);
}

}  // namespace
