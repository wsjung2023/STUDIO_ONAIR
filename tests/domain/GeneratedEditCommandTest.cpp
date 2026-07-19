#include "domain/GeneratedClipCommands.h"

#include "core/Timebase.h"
#include "domain/EditHistory.h"
#include "domain/Timeline.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AddCaptionCueCommand;
using creator::domain::AddTitleCommand;
using creator::domain::CaptionCue;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::CueId;
using creator::domain::EditCaptionCueCommand;
using creator::domain::EditHistory;
using creator::domain::EditTitleCommand;
using creator::domain::RemoveCaptionCueCommand;
using creator::domain::RemoveGeneratedClipCommand;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TitlePayload;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

CommandId commandId(std::string value) {
    return CommandId::create(std::move(value)).value();
}

ClipId clipId(std::string value) {
    return ClipId::create(std::move(value)).value();
}

CueId cueId(std::string value) {
    return CueId::create(std::move(value)).value();
}

TrackId titleTrackId() {
    return TrackId::create("title-1").value();
}

TrackId captionTrackId() {
    return TrackId::create("caption-1").value();
}

Timeline emptyTimeline() {
    return Timeline::create(TimelineId::create("timeline").value(), "Main",
                            FrameRate::create(60, 1).value()).value();
}

TitlePayload title(std::string text) {
    return TitlePayload::create(
               std::move(text), "Malgun Gothic", 0.5, 0.9,
               RgbaColor::parse("#ffffffff").value(),
               RgbaColor::parse("#00000080").value(),
               TextAlignment::Center)
        .value();
}

Clip titleClip(std::string id, std::string text,
               std::int64_t start = 0, std::int64_t duration = 30) {
    return Clip::createTitle(
               clipId(std::move(id)),
               TimeRange::create(at(start), DurationNs{duration}).value(),
               true, title(std::move(text)), std::nullopt)
        .value();
}

CaptionCue cue(std::string id, std::int64_t start,
               std::int64_t duration, std::string text) {
    return CaptionCue::create(cueId(std::move(id)), DurationNs{start},
                              DurationNs{duration}, std::move(text)).value();
}

TimeRange captionRange() {
    return TimeRange::create(at(100), DurationNs{100}).value();
}

TEST(GeneratedEditCommandTest, AddTitleCreatesStableTrackAndUndoRemovesBoth) {
    auto value = emptyTimeline();
    const auto before = value;
    AddTitleCommand command{commandId("add-title"), titleTrackId(), "Titles",
                            titleClip("title-a", "첫 제목")};

    ASSERT_TRUE(command.execute(value).hasValue());
    ASSERT_NE(value.track(titleTrackId()), nullptr);
    ASSERT_EQ(value.track(titleTrackId())->clips().size(), 1U);
    EXPECT_EQ(command.record().type, "ADD_TITLE");
    EXPECT_NE(command.record().payload.find("\"version\":1"),
              std::string::npos);
    EXPECT_NE(command.record().payload.find("첫 제목"), std::string::npos);
    ASSERT_TRUE(command.undo(value).hasValue());
    EXPECT_EQ(value, before);
}

TEST(GeneratedEditCommandTest, AddTitleUndoKeepsPreexistingTrack) {
    auto value = emptyTimeline();
    ASSERT_TRUE(value.addTrack(Track::create(
        titleTrackId(), TrackKind::Title, "Titles", true, false).value()).hasValue());
    AddTitleCommand command{commandId("add-title"), titleTrackId(), "Titles",
                            titleClip("title-a", "제목")};

    ASSERT_TRUE(command.execute(value).hasValue());
    ASSERT_TRUE(command.undo(value).hasValue());

    ASSERT_NE(value.track(titleTrackId()), nullptr);
    EXPECT_TRUE(value.track(titleTrackId())->clips().empty());
}

TEST(GeneratedEditCommandTest, EditAndRemoveTitleRoundTripExactly) {
    auto value = emptyTimeline();
    ASSERT_TRUE(value.addTrack(Track::create(
        titleTrackId(), TrackKind::Title, "Titles", true, false).value()).hasValue());
    ASSERT_TRUE(value.insertClip(titleTrackId(),
                                 titleClip("title-a", "원래 제목")).hasValue());
    const auto before = value;
    EditTitleCommand edit{commandId("edit-title"), titleTrackId(),
                          clipId("title-a"), title("수정 제목")};

    ASSERT_TRUE(edit.execute(value).hasValue());
    EXPECT_EQ(value.clip(titleTrackId(), clipId("title-a"))
                  ->titlePayload()->text(), "수정 제목");
    ASSERT_TRUE(edit.undo(value).hasValue());
    EXPECT_EQ(value, before);

    RemoveGeneratedClipCommand remove{commandId("remove-title"),
                                      titleTrackId(), clipId("title-a")};
    ASSERT_TRUE(remove.execute(value).hasValue());
    EXPECT_EQ(value.clip(titleTrackId(), clipId("title-a")), nullptr);
    ASSERT_TRUE(remove.undo(value).hasValue());
    EXPECT_EQ(value, before);
}

TEST(GeneratedEditCommandTest, FirstCaptionCueCreatesTrackAndClipExactly) {
    auto value = emptyTimeline();
    const auto before = value;
    AddCaptionCueCommand command{
        commandId("add-cue"), captionTrackId(), "Captions",
        clipId("caption-a"), captionRange(), true, std::nullopt,
        cue("cue-a", 0, 20, "첫 자막")};

    ASSERT_TRUE(command.execute(value).hasValue());
    const auto* caption = value.clip(captionTrackId(), clipId("caption-a"));
    ASSERT_NE(caption, nullptr);
    ASSERT_EQ(caption->captionCues().size(), 1U);
    EXPECT_EQ(caption->captionCues().front().text(), "첫 자막");
    EXPECT_EQ(command.record().type, "ADD_CAPTION_CUE");
    EXPECT_NE(command.record().payload.find("\"version\":1"),
              std::string::npos);
    ASSERT_TRUE(command.undo(value).hasValue());
    EXPECT_EQ(value, before);
}

TEST(GeneratedEditCommandTest, AdditionalCueUndoKeepsExistingClipAndTrack) {
    auto value = emptyTimeline();
    AddCaptionCueCommand first{
        commandId("first"), captionTrackId(), "Captions",
        clipId("caption-a"), captionRange(), true, std::nullopt,
        cue("cue-a", 0, 20, "첫 자막")};
    ASSERT_TRUE(first.execute(value).hasValue());
    const auto afterFirst = value;
    AddCaptionCueCommand second{
        commandId("second"), captionTrackId(), "Captions",
        clipId("caption-a"), captionRange(), true, std::nullopt,
        cue("cue-b", 30, 20, "둘째 자막")};

    ASSERT_TRUE(second.execute(value).hasValue());
    ASSERT_EQ(value.clip(captionTrackId(), clipId("caption-a"))
                  ->captionCues().size(), 2U);
    ASSERT_TRUE(second.undo(value).hasValue());
    EXPECT_EQ(value, afterFirst);
}

TEST(GeneratedEditCommandTest, CaptionOverlapFailsWithoutMutationOrHistory) {
    auto value = emptyTimeline();
    EditHistory history{10};
    ASSERT_TRUE(history.execute(
        value, std::make_unique<AddCaptionCueCommand>(
            commandId("first"), captionTrackId(), "Captions",
            clipId("caption-a"), captionRange(), true, std::nullopt,
            cue("cue-a", 0, 30, "first"))).hasValue());
    const auto before = value;
    const auto cursor = history.cursor();

    const auto result = history.execute(
        value, std::make_unique<AddCaptionCueCommand>(
            commandId("overlap"), captionTrackId(), "Captions",
            clipId("caption-a"), captionRange(), true, std::nullopt,
            cue("cue-b", 20, 30, "overlap")));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(value, before);
    EXPECT_EQ(history.cursor(), cursor);
}

TEST(GeneratedEditCommandTest, EditAndRemoveCaptionCueRestoreExactState) {
    auto value = emptyTimeline();
    AddCaptionCueCommand first{
        commandId("first"), captionTrackId(), "Captions",
        clipId("caption-a"), captionRange(), true, std::nullopt,
        cue("cue-a", 0, 20, "first")};
    AddCaptionCueCommand second{
        commandId("second"), captionTrackId(), "Captions",
        clipId("caption-a"), captionRange(), true, std::nullopt,
        cue("cue-b", 30, 20, "second")};
    ASSERT_TRUE(first.execute(value).hasValue());
    ASSERT_TRUE(second.execute(value).hasValue());
    const auto before = value;

    EditCaptionCueCommand edit{
        commandId("edit"), captionTrackId(), clipId("caption-a"),
        cueId("cue-b"), cue("cue-b", 40, 20, "수정")};
    ASSERT_TRUE(edit.execute(value).hasValue());
    EXPECT_EQ(value.clip(captionTrackId(), clipId("caption-a"))
                  ->captionCues()[1].text(), "수정");
    ASSERT_TRUE(edit.undo(value).hasValue());
    EXPECT_EQ(value, before);

    RemoveCaptionCueCommand remove{
        commandId("remove"), captionTrackId(), clipId("caption-a"),
        cueId("cue-b")};
    ASSERT_TRUE(remove.execute(value).hasValue());
    ASSERT_EQ(value.clip(captionTrackId(), clipId("caption-a"))
                  ->captionCues().size(), 1U);
    ASSERT_TRUE(remove.undo(value).hasValue());
    EXPECT_EQ(value, before);
}

TEST(GeneratedEditCommandTest, RemovingLastCueRemovesOnlyClipAndUndoRestoresIt) {
    auto value = emptyTimeline();
    ASSERT_TRUE(value.addTrack(Track::create(
        captionTrackId(), TrackKind::Caption,
        "Captions", true, false).value()).hasValue());
    ASSERT_TRUE(value.insertClip(
        captionTrackId(), Clip::createCaption(
            clipId("caption-a"), captionRange(), true,
            {cue("cue-a", 0, 20, "only")}, std::nullopt).value()).hasValue());
    const auto before = value;
    RemoveCaptionCueCommand remove{
        commandId("remove-last"), captionTrackId(), clipId("caption-a"),
        cueId("cue-a")};

    ASSERT_TRUE(remove.execute(value).hasValue());
    EXPECT_EQ(value.clip(captionTrackId(), clipId("caption-a")), nullptr);
    EXPECT_NE(value.track(captionTrackId()), nullptr);
    ASSERT_TRUE(remove.undo(value).hasValue());
    EXPECT_EQ(value, before);
}

TEST(GeneratedEditCommandTest, GeneratedCommandsRejectWrongStableTrackAndLock) {
    auto value = emptyTimeline();
    const auto before = value;
    AddTitleCommand wrong{commandId("wrong"),
                          TrackId::create("titles-random").value(), "Titles",
                          titleClip("title-a", "wrong")};
    EXPECT_FALSE(wrong.execute(value).hasValue());
    EXPECT_EQ(value, before);

    ASSERT_TRUE(value.addTrack(Track::create(
        titleTrackId(), TrackKind::Title, "Titles", true, true).value()).hasValue());
    const auto lockedBefore = value;
    AddTitleCommand locked{commandId("locked"), titleTrackId(), "Titles",
                            titleClip("title-a", "locked")};
    EXPECT_FALSE(locked.execute(value).hasValue());
    EXPECT_EQ(value, lockedBefore);
}

TEST(GeneratedEditCommandTest, GeneratedCommandHistoryCopiesUndoAndRedoExactly) {
    auto value = emptyTimeline();
    const auto empty = value;
    EditHistory history{10};
    ASSERT_TRUE(history.execute(
        value, std::make_unique<AddTitleCommand>(
            commandId("add-title"), titleTrackId(), "Titles",
            titleClip("title-a", "이력 제목"))).hasValue());
    const auto added = value;

    ASSERT_TRUE(history.undo(value).hasValue());
    EXPECT_EQ(value, empty);
    ASSERT_TRUE(history.redo(value).hasValue());
    EXPECT_EQ(value, added);

    auto copiedHistory = history;
    auto copiedTimeline = value;
    ASSERT_TRUE(copiedHistory.undo(copiedTimeline).hasValue());
    EXPECT_EQ(copiedTimeline, empty);
    EXPECT_EQ(value, added);
    EXPECT_EQ(history.cursor(), 1U);
}

}  // namespace
