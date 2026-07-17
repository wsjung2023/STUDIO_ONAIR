#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/UnavailableEditEngine.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineRevision;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;
using creator::domain::TimeRange;
using creator::domain::TitlePayload;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::edit_engine::GeneratedOverlayDescriptor;
using creator::edit_engine::PreviewFrame;
using creator::edit_engine::RenderJobState;
using creator::edit_engine::RenderPreset;
using creator::edit_engine::RenderProgress;
using creator::edit_engine::RenderRequest;
using creator::edit_engine::TimelineChangeSet;
using creator::edit_engine::TimelineSnapshot;
using creator::edit_engine::UnavailableEditEngine;
using creator::media::PixelFormat;
using creator::media::VideoFrame;

Timeline timeline(std::string name = "Main") {
    return Timeline::create(TimelineId::create("main").value(), std::move(name),
                            FrameRate::create(60, 1).value())
        .value();
}

TimelineSnapshot snapshot(std::int64_t revision = 1) {
    return TimelineSnapshot{timeline(), TimelineRevision::create(revision).value()};
}

TimelineSnapshot generatedSnapshot() {
    auto value = timeline();
    const auto trackId = TrackId::create("title-1").value();
    EXPECT_TRUE(value.addTrack(
                         Track::create(trackId, TrackKind::Title, "Titles", true,
                                       false)
                             .value())
                    .hasValue());
    const auto range = TimeRange::create(TimestampNs{DurationNs{100}},
                                         DurationNs{500})
                           .value();
    const auto payload = TitlePayload::create(
        "Hello", "Creator Sans", 0.5, 0.5,
        RgbaColor::parse("#ffffffff").value(),
        RgbaColor::parse("#00000000").value(), TextAlignment::Center)
                             .value();
    EXPECT_TRUE(value.insertClip(
                         trackId,
                         Clip::createTitle(ClipId::create("title-clip").value(),
                                           range, true, payload, std::nullopt)
                             .value())
                    .hasValue());
    return TimelineSnapshot{std::move(value),
                            TimelineRevision::create(1).value(), {}, {}, 1080,
                            1920};
}

VideoFrame frameAt(std::int64_t nanoseconds) {
    return VideoFrame{
        .timestamp = TimestampNs{DurationNs{nanoseconds}},
        .width = 1920,
        .height = 1080,
        .visibleRect = {.x = 0, .y = 0, .width = 1920, .height = 1080},
        .contentWidth = 1920,
        .contentHeight = 1080,
        .contentScale = 1.0,
        .pointPixelScale = 1.0,
        .pixelFormat = PixelFormat::Bgra8,
    };
}

TEST(UnavailableEditEngineTest, RejectsEveryPlaybackCommandExplicitly) {
    UnavailableEditEngine engine;
    const auto position = TimestampNs{DurationNs{100}};

    const auto load = engine.load(snapshot());
    const auto play = engine.play();
    const auto pause = engine.pause();
    const auto seek = engine.seek(position);
    const auto frame = engine.requestFrame(position);

    ASSERT_FALSE(load.hasValue());
    EXPECT_EQ(load.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(load.error().message(),
              "Editor playback engine is unavailable in this build");
    EXPECT_FALSE(play.hasValue());
    EXPECT_FALSE(pause.hasValue());
    EXPECT_FALSE(seek.hasValue());
    EXPECT_FALSE(frame.hasValue());
}

TEST(EditEngineTypesTest, CreatesSequentialUniqueTimelineChangeSet) {
    auto change = TimelineChangeSet::create(
        TimelineRevision::create(1).value(), snapshot(2),
        {TrackId::create("screen").value(), TrackId::create("camera").value()},
        false);

    ASSERT_TRUE(change.hasValue()) << change.error().message();
    EXPECT_EQ(change.value().baseRevision().value(), 1);
    EXPECT_EQ(change.value().target().revision.value(), 2);
    EXPECT_EQ(change.value().affectedTracks().size(), 2U);
    EXPECT_FALSE(change.value().requiresFullRebuild());
}

TEST(EditEngineTypesTest, RejectsRevisionGapDuplicateTracksAndEmptyIncrementalSet) {
    const auto duplicate = TrackId::create("screen").value();
    auto gap = TimelineChangeSet::create(
        TimelineRevision::create(1).value(), snapshot(3), {duplicate}, false);
    auto duplicates = TimelineChangeSet::create(
        TimelineRevision::create(1).value(), snapshot(2), {duplicate, duplicate}, false);
    auto empty = TimelineChangeSet::create(
        TimelineRevision::create(1).value(), snapshot(2), {}, false);

    ASSERT_FALSE(gap.hasValue());
    ASSERT_FALSE(duplicates.hasValue());
    ASSERT_FALSE(empty.hasValue());
    EXPECT_EQ(gap.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(duplicates.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);
}

TEST(EditEngineTypesTest, BoundsAffectedTrackWork) {
    std::vector<TrackId> tracks;
    for (std::size_t index = 0; index <= TimelineChangeSet::kMaxAffectedTracks;
         ++index) {
        tracks.push_back(
            TrackId::create("track-" + std::to_string(index)).value());
    }

    auto result = TimelineChangeSet::create(
        TimelineRevision::create(1).value(), snapshot(2), std::move(tracks), false);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(EditEngineTypesTest, ValidatesCanvasAndGeneratedOverlayOwnership) {
    auto valid = generatedSnapshot();
    valid.generatedOverlays.push_back(
        GeneratedOverlayDescriptor::create(
            ClipId::create("title-clip").value(), std::nullopt,
            std::filesystem::path{"cache/generated/title-clip.png"},
            TimeRange::create(TimestampNs{DurationNs{150}}, DurationNs{100})
                .value(),
            "Creator Sans")
            .value());

    EXPECT_TRUE(creator::edit_engine::validateTimelineSnapshot(valid).hasValue());

    auto escaped = GeneratedOverlayDescriptor::create(
        ClipId::create("title-clip").value(), std::nullopt,
        std::filesystem::path{"cache/generated/../outside.png"},
        TimeRange::create(TimestampNs{DurationNs{150}}, DurationNs{100}).value(),
        "Creator Sans");
    EXPECT_FALSE(escaped.hasValue());
    EXPECT_FALSE(GeneratedOverlayDescriptor::create(
                     ClipId::create("title-clip").value(), std::nullopt,
                     std::filesystem::path{"cache/generated"},
                     TimeRange::create(TimestampNs{DurationNs{150}},
                                       DurationNs{100})
                         .value(),
                     "Creator Sans")
                     .hasValue());

    auto outside = generatedSnapshot();
    outside.generatedOverlays.push_back(
        GeneratedOverlayDescriptor::create(
            ClipId::create("title-clip").value(), std::nullopt,
            std::filesystem::path{"cache/generated/title-clip.png"},
            TimeRange::create(TimestampNs{DurationNs{550}}, DurationNs{100})
                .value(),
            "Creator Sans")
            .value());
    EXPECT_FALSE(
        creator::edit_engine::validateTimelineSnapshot(outside).hasValue());

    auto invalidCanvas = generatedSnapshot();
    invalidCanvas.canvasWidth = 15;
    EXPECT_FALSE(creator::edit_engine::validateTimelineSnapshot(invalidCanvas)
                     .hasValue());
}

TEST(EditEngineTypesTest, CreatesValidatedPreviewFrame) {
    auto preview = PreviewFrame::create(
        TimestampNs{DurationNs{250}}, TimelineRevision::create(4).value(),
        frameAt(250));

    ASSERT_TRUE(preview.hasValue()) << preview.error().message();
    EXPECT_EQ(preview.value().position().time_since_epoch().count(), 250);
    EXPECT_EQ(preview.value().revision().value(), 4);
    EXPECT_EQ(preview.value().frame().width, 1920U);
}

TEST(EditEngineTypesTest, RejectsPreviewTimestampMismatchAndInvalidSurface) {
    auto mismatch = PreviewFrame::create(
        TimestampNs{DurationNs{250}}, TimelineRevision::create(4).value(),
        frameAt(251));
    auto invalidFrame = frameAt(250);
    invalidFrame.visibleRect.width = 1921;
    auto invalid = PreviewFrame::create(
        TimestampNs{DurationNs{250}}, TimelineRevision::create(4).value(),
        std::move(invalidFrame));

    EXPECT_FALSE(mismatch.hasValue());
    EXPECT_FALSE(invalid.hasValue());
}

TEST(EditEngineTypesTest, ValidatesRenderPresetRequestAndProgress) {
    auto preset = RenderPreset::create(
        1920, 1080, FrameRate::create(60, 1).value(), 12'000'000, 192'000);
    ASSERT_TRUE(preset.hasValue()) << preset.error().message();
    auto request = RenderRequest::create(
        snapshot(7), std::filesystem::path{"D:/Exports/tutorial.mp4"},
        preset.value());
    ASSERT_TRUE(request.hasValue()) << request.error().message();
    auto progress = RenderProgress::create(
        RenderJobState::Running, 0.5, TimestampNs{DurationNs{500}},
        DurationNs{1000});
    ASSERT_TRUE(progress.hasValue()) << progress.error().message();
    EXPECT_DOUBLE_EQ(progress.value().fraction(), 0.5);
}

TEST(EditEngineTypesTest, RejectsUnsafeRenderValuesAndImpossibleProgress) {
    auto invalidPreset = RenderPreset::create(
        0, 1080, FrameRate::create(60, 1).value(), 12'000'000, 192'000);
    ASSERT_FALSE(invalidPreset.hasValue());
    auto preset = RenderPreset::create(
        1920, 1080, FrameRate::create(60, 1).value(), 12'000'000, 192'000)
                      .value();
    auto emptyDestination = RenderRequest::create(snapshot(), {}, preset);
    auto traversalName = RenderRequest::create(
        snapshot(), std::filesystem::path{"D:/Exports/../tutorial.mp4"}, preset);
    auto impossible = RenderProgress::create(
        RenderJobState::Completed, 0.5, TimestampNs{DurationNs{500}},
        DurationNs{1000});

    EXPECT_FALSE(emptyDestination.hasValue());
    EXPECT_FALSE(traversalName.hasValue());
    EXPECT_FALSE(impossible.hasValue());
}

}  // namespace
