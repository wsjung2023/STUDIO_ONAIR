#include "app/RecordingImportPlanner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using creator::app::RecordingImportRequest;
using creator::app::RecordingConcatSource;
using creator::app::RecordingSegmentProbe;
using creator::app::planRecordingImport;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::SceneId;
using creator::domain::SceneSource;
using creator::domain::SegmentInfo;
using creator::domain::SegmentStatus;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::domain::StudioScene;
using creator::domain::StudioSourceRole;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VisualTransform;
using creator::media::MediaProbeResult;
using creator::project_store::RecordingMarker;
using creator::project_store::RecordingSceneEvent;
using creator::project_store::RecordingSourceRole;

TimestampNs at(DurationNs value) { return TimestampNs{value}; }

SourceId source(std::string value) {
    return SourceId::create(std::move(value)).value();
}

SceneId sceneId(std::string value) {
    return SceneId::create(std::move(value)).value();
}

VisualTransform transform(double x, std::int32_t z) {
    return VisualTransform::create(x, 0.1, 0.4, 0.4, 1.0, 1.0, 0.0,
                                   0.0, 0.0, 0.0, 0.0, 1.0, z)
        .value();
}

StudioScene scene(std::string id, const SourceId& idOfSource,
                  StudioSourceRole role, bool enabled,
                  std::optional<VisualTransform> visual = std::nullopt) {
    auto sceneSource = SceneSource::create(idOfSource, role, "Recorded source",
                                           0, enabled, std::move(visual));
    EXPECT_TRUE(sceneSource.hasValue());
    return StudioScene::create(sceneId(std::move(id)), "Scene", 0,
                               {sceneSource.value()})
        .value();
}

SegmentInfo segment(const SourceId& id, std::uint64_t index,
                    DurationNs start, DurationNs duration,
                    std::string path,
                    SegmentStatus status = SegmentStatus::Ready) {
    return SegmentInfo{.index = index,
                       .sourceId = id,
                       .startTime = at(start),
                       .duration = duration,
                       .status = status,
                       .relativePath = std::move(path)};
}

MediaProbeResult videoProbe(DurationNs duration) {
    return MediaProbeResult{
        .duration = duration,
        .video = creator::domain::VideoAssetMetadata{
            .width = 1920,
            .height = 1080,
            .frameRate = creator::core::FrameRate::create(30, 1).value()},
        .audio = std::nullopt,
        .formatName = "matroska",
        .codecName = "h264",
        .byteSize = 4'096,
        .sha256 = std::string(64, 'a')};
}

MediaProbeResult audioProbe(DurationNs duration) {
    return MediaProbeResult{
        .duration = duration,
        .video = std::nullopt,
        .audio = creator::domain::AudioAssetMetadata{.sampleRate = 48'000,
                                                     .channels = 1},
        .formatName = "matroska",
        .codecName = "aac",
        .byteSize = 2'048,
        .sha256 = std::string(64, 'b')};
}

Timeline emptyTimeline() {
    return Timeline::create(TimelineId::create("main").value(), "Main",
                            creator::core::FrameRate::create(30, 1).value())
        .value();
}

Timeline timelineEndingAt(DurationNs end,
                          std::string assetIdentity = "existing-asset") {
    auto value = emptyTimeline();
    const auto asset = MediaAsset::create(
        AssetId::create(std::move(assetIdentity)).value(), MediaKind::Video,
        "media/existing.mkv", end,
        creator::domain::VideoAssetMetadata{
            .width = 16,
            .height = 16,
            .frameRate = creator::core::FrameRate::create(30, 1).value()},
        std::nullopt, 10, "existing", AssetAvailability::Available)
                           .value();
    const auto trackId = TrackId::create("existing-track").value();
    EXPECT_TRUE(value.addTrack(
                         Track::create(trackId, TrackKind::Video, "Existing",
                                       true, false)
                             .value())
                    .hasValue());
    EXPECT_TRUE(value.insertClip(
                         trackId,
                         Clip::createAsset(
                             ClipId::create("existing-clip").value(), asset,
                             TimeRange::create({}, end).value(),
                             TimeRange::create({}, end).value(), true,
                             std::nullopt, std::nullopt)
                             .value())
                    .hasValue());
    return value;
}

RecordingImportRequest request(
    const SourceId& id, StudioSourceRole role,
    std::vector<SegmentInfo> segments, std::vector<StudioScene> scenes,
    std::vector<RecordingSceneEvent> events,
    std::vector<RecordingSegmentProbe> probes,
    std::vector<RecordingMarker> markers = {},
    Timeline timeline = emptyTimeline()) {
    const auto session = SessionId::create("세션-α").value();
    return RecordingImportRequest{
        .sessionId = session,
        .segments = std::move(segments),
        .sources = {RecordingSourceRole{.sourceId = id, .role = role}},
        .scenes = std::move(scenes),
        .sceneEvents = std::move(events),
        .markers = std::move(markers),
        .timeline = std::move(timeline),
        .probes = std::move(probes)};
}

TEST(RecordingImportPlannerTest, SplitsVideoAtInteriorSceneBoundariesExactly) {
    const auto camera = source("카메라-α");
    const auto session = SessionId::create("세션-α").value();
    const auto presentation = scene("presentation", camera,
                                    StudioSourceRole::Camera, true,
                                    transform(0.6, 2));
    const auto hidden = scene("hidden", camera, StudioSourceRole::Camera,
                              false, transform(0.2, 2));
    auto input = request(
        camera, StudioSourceRole::Camera,
        {segment(camera, 0, 0s, 2s, "media/camera-0.mkv"),
         segment(camera, 1, 2s, 2s, "media/camera-1.mkv")},
        {presentation, hidden},
        {{.sessionId = session,
          .sequence = 2,
          .sceneId = presentation.id(),
          .position = at(3s)},
         {.sessionId = session,
          .sequence = 0,
          .sceneId = presentation.id(),
          .position = at(0s)},
         {.sessionId = session,
          .sequence = 1,
          .sceneId = hidden.id(),
          .position = at(1500ms)}},
        {{.relativePath = "media/camera-1.mkv", .media = videoProbe(2s)},
         {.relativePath = "media/camera-0.mkv", .media = videoProbe(2s)}});

    const auto planned = planRecordingImport(input);

    ASSERT_TRUE(planned.hasValue()) << planned.error().message();
    ASSERT_EQ(planned.value().assets.size(), 2U);
    ASSERT_EQ(planned.value().tracks.size(), 1U);
    const auto& clips = planned.value().tracks.front().clips();
    ASSERT_EQ(clips.size(), 4U);
    EXPECT_EQ(clips[0].timelineRange(), TimeRange::create(at(0s), 1500ms).value());
    EXPECT_EQ(clips[1].timelineRange(), TimeRange::create(at(1500ms), 500ms).value());
    EXPECT_EQ(clips[2].timelineRange(), TimeRange::create(at(2s), 1s).value());
    EXPECT_EQ(clips[3].timelineRange(), TimeRange::create(at(3s), 1s).value());
    EXPECT_TRUE(clips[0].enabled());
    EXPECT_FALSE(clips[1].enabled());
    EXPECT_FALSE(clips[2].enabled());
    EXPECT_TRUE(clips[3].enabled());
    EXPECT_EQ(clips[0].visualTransform(), presentation.sources()[0].transform());
    EXPECT_EQ(clips[1].sourceRange(), TimeRange::create(at(1500ms), 500ms).value());
    EXPECT_EQ(clips[2].sourceRange(), TimeRange::create(at(0s), 1s).value());
}

TEST(RecordingImportPlannerTest,
     MergesAdjacentSegmentsIntoOneAssetAndClipPerContinuousTake) {
    const auto camera = source("camera-merge");
    const auto active = scene("active", camera, StudioSourceRole::Camera, true,
                              transform(0.6, 2));
    auto input = request(
        camera, StudioSourceRole::Camera,
        {segment(camera, 0, 0s, 2s, "media/camera-0.mkv"),
         segment(camera, 1, 2s, 2s, "media/camera-1.mkv"),
         segment(camera, 2, 4s, 2s, "media/camera-2.mkv")},
        {active},
        {{.sessionId = SessionId::create("session-placeholder").value(),
          .sequence = 0,
          .sceneId = active.id(),
          .position = at(0s)}},
        {{.relativePath = "media/camera-0.mkv", .media = videoProbe(2s)},
         {.relativePath = "media/camera-1.mkv", .media = videoProbe(2s)},
         {.relativePath = "media/camera-2.mkv", .media = videoProbe(2s)}});
    input.sceneEvents.front().sessionId = input.sessionId;
    input.concatSources.push_back(RecordingConcatSource{
        .sourceId = camera,
        .relativePath = "concat-camera-merge.ffconcat",
        .media = videoProbe(6s),
        .entries = {{"media/camera-0.mkv", 0s},
                    {"media/camera-1.mkv", 2s},
                    {"media/camera-2.mkv", 4s}}});

    const auto planned = planRecordingImport(input);

    ASSERT_TRUE(planned.hasValue()) << planned.error().message();
    ASSERT_EQ(planned.value().assets.size(), 1U);
    ASSERT_EQ(planned.value().tracks.size(), 1U);
    ASSERT_EQ(planned.value().tracks.front().clips().size(), 1U);
    EXPECT_EQ(planned.value().assets.front().relativePath(),
              "concat-camera-merge.ffconcat");
    EXPECT_EQ(planned.value().tracks.front().clips().front().sourceRange(),
              TimeRange::create(at(0s), 6s).value());
}

TEST(RecordingImportPlannerTest,
     NormalizesSubSampleConcatDurationRoundingButRejectsRealTruncation) {
    const auto microphone = source("microphone-concat-rounding");
    const auto active = scene("active-audio", microphone,
                              StudioSourceRole::Microphone, true);
    auto input = request(
        microphone, StudioSourceRole::Microphone,
        {segment(microphone, 0, 0s, 2s, "media/mic-0.mka"),
         segment(microphone, 1, 2s, 2s, "media/mic-1.mka"),
         segment(microphone, 2, 4s, 2s, "media/mic-2.mka")},
        {active},
        {{.sessionId = SessionId::create("session-placeholder").value(),
          .sequence = 0,
          .sceneId = active.id(),
          .position = at(0s)}},
        {{.relativePath = "media/mic-0.mka", .media = audioProbe(2s)},
         {.relativePath = "media/mic-1.mka", .media = audioProbe(2s)},
         {.relativePath = "media/mic-2.mka", .media = audioProbe(2s)}});
    input.sceneEvents.front().sessionId = input.sessionId;
    input.concatSources.push_back(RecordingConcatSource{
        .sourceId = microphone,
        .relativePath = "derived-concat-microphone.ffconcat",
        .media = audioProbe(6s - 200ns),
        .entries = {{"media/mic-0.mka", 0s},
                    {"media/mic-1.mka", 2s},
                    {"media/mic-2.mka", 4s}}});

    const auto rounded = planRecordingImport(input);

    ASSERT_TRUE(rounded.hasValue()) << rounded.error().message();
    ASSERT_EQ(rounded.value().assets.size(), 1U);
    EXPECT_EQ(rounded.value().assets.front().duration(), 6s);

    input.concatSources.front().media.duration = 6s - 1ms;
    const auto truncated = planRecordingImport(input);
    ASSERT_FALSE(truncated.hasValue());
    EXPECT_EQ(truncated.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingImportPlannerTest,
     MergesFrameBoundaryGapsWithoutCollapsingTheirTimelineOffsets) {
    const auto screen = source("screen-gapped-concat");
    const auto active = scene("active-screen", screen,
                              StudioSourceRole::Screen, true,
                              transform(1.0, 0));
    auto input = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1982ms, "media/screen-0.mkv"),
         segment(screen, 1, 2s, 1982ms, "media/screen-1.mkv"),
         segment(screen, 2, 4s, 1982ms, "media/screen-2.mkv")},
        {active},
        {{.sessionId = SessionId::create("session-placeholder").value(),
          .sequence = 0,
          .sceneId = active.id(),
          .position = at(0s)}},
        {{.relativePath = "media/screen-0.mkv", .media = videoProbe(1982ms)},
         {.relativePath = "media/screen-1.mkv", .media = videoProbe(1982ms)},
         {.relativePath = "media/screen-2.mkv", .media = videoProbe(1982ms)}});
    input.sceneEvents.front().sessionId = input.sessionId;
    input.concatSources.push_back(RecordingConcatSource{
        .sourceId = screen,
        .relativePath = "derived-concat-screen.ffconcat",
        .media = videoProbe(5982ms),
        .entries = {{"media/screen-0.mkv", 0s},
                    {"media/screen-1.mkv", 2s},
                    {"media/screen-2.mkv", 4s}}});

    const auto planned = planRecordingImport(input);

    ASSERT_TRUE(planned.hasValue()) << planned.error().message();
    ASSERT_EQ(planned.value().assets.size(), 1U);
    ASSERT_EQ(planned.value().tracks.front().clips().size(), 1U);
    EXPECT_EQ(planned.value().assets.front().duration(), 5982ms);
    EXPECT_EQ(planned.value().tracks.front().clips().front().sourceRange(),
              TimeRange::create(at(0s), 5982ms).value());

    input.concatSources.front().entries[1].offset = 1982ms;
    const auto collapsedGap = planRecordingImport(input);
    ASSERT_FALSE(collapsedGap.hasValue());
    EXPECT_EQ(collapsedGap.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingImportPlannerTest, AudioSplitsOnlyWhenSceneEnableStateChanges) {
    const auto microphone = source("microphone");
    const auto session = SessionId::create("세션-α").value();
    const auto enabledA = scene("enabled-a", microphone,
                                StudioSourceRole::Microphone, true);
    const auto enabledB = scene("enabled-b", microphone,
                                StudioSourceRole::Microphone, true);
    const auto disabled = scene("disabled", microphone,
                                StudioSourceRole::Microphone, false);
    auto input = request(
        microphone, StudioSourceRole::Microphone,
        {segment(microphone, 0, 0s, 4s, "media/mic.mka")},
        {enabledA, enabledB, disabled},
        {{session, 2, disabled.id(), at(2s)},
         {session, 0, enabledA.id(), at(0s)},
         {session, 3, disabled.id(), at(3s)},
         {session, 1, enabledB.id(), at(1s)}},
        {{"media/mic.mka", audioProbe(4s)}});

    const auto planned = planRecordingImport(input);

    ASSERT_TRUE(planned.hasValue()) << planned.error().message();
    const auto& clips = planned.value().tracks.front().clips();
    ASSERT_EQ(clips.size(), 2U);
    EXPECT_EQ(clips[0].timelineRange(), TimeRange::create(at(0s), 2s).value());
    EXPECT_EQ(clips[1].timelineRange(), TimeRange::create(at(2s), 2s).value());
    EXPECT_TRUE(clips[0].enabled());
    EXPECT_FALSE(clips[1].enabled());
    EXPECT_TRUE(clips[0].audioEnvelope().has_value());
}

TEST(RecordingImportPlannerTest, PreservesFailedSegmentGapAndBoundarySwitch) {
    const auto screen = source("screen");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", screen, StudioSourceRole::Screen,
                               true, transform(0.0, 0));
    const auto hidden = scene("hidden", screen, StudioSourceRole::Screen,
                              false, transform(0.0, 0));
    auto input = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/s0.mkv"),
         segment(screen, 1, 1s, 1s, "media/s1.mkv", SegmentStatus::Failed),
         segment(screen, 2, 2s, 1s, "media/s2.mkv")},
        {visible, hidden},
        {{session, 0, visible.id(), at(0s)},
         {session, 1, hidden.id(), at(2s)}},
        {{"media/s0.mkv", videoProbe(1s)},
         {"media/s2.mkv", videoProbe(1s)}});

    const auto planned = planRecordingImport(input);

    ASSERT_TRUE(planned.hasValue()) << planned.error().message();
    const auto& clips = planned.value().tracks.front().clips();
    ASSERT_EQ(clips.size(), 2U);
    EXPECT_EQ(clips[0].timelineRange(), TimeRange::create(at(0s), 1s).value());
    EXPECT_EQ(clips[1].timelineRange(), TimeRange::create(at(2s), 1s).value());
    EXPECT_FALSE(clips[1].enabled());
}

TEST(RecordingImportPlannerTest, OffsetsMarkersAtTimelineEndAndRepeatsExactly) {
    const auto screen = source("화면");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", screen, StudioSourceRole::Screen,
                               true, transform(0.0, 0));
    auto input = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/화면.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/화면.mkv", videoProbe(1s)}},
        {{"마커-2", session, at(750ms), "두 번째"},
         {"마커-1", session, at(250ms), "첫 번째"}},
        timelineEndingAt(5s));

    const auto first = planRecordingImport(input);
    const auto second = planRecordingImport(input);

    ASSERT_TRUE(first.hasValue()) << first.error().message();
    ASSERT_TRUE(second.hasValue()) << second.error().message();
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(first.value().appendBase, at(5s));
    ASSERT_EQ(first.value().markers.size(), 2U);
    EXPECT_EQ(first.value().markers[0].position(), at(5250ms));
    EXPECT_EQ(first.value().markers[1].position(), at(5750ms));
    EXPECT_EQ(first.value().tracks.front().clips().front().timelineRange().start(),
              at(5s));
}

TEST(RecordingImportPlannerTest, RejectsMissingRoleProbeAndOverflow) {
    const auto camera = source("camera");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", camera, StudioSourceRole::Camera,
                               true, transform(0.5, 1));
    auto missingProbe = request(
        camera, StudioSourceRole::Camera,
        {segment(camera, 0, 0s, 1s, "media/camera.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}}, {});
    auto missingRole = missingProbe;
    missingRole.probes = {{"media/camera.mkv", videoProbe(1s)}};
    missingRole.sources.clear();
    auto overflow = request(
        camera, StudioSourceRole::Camera,
        {segment(camera, 0, 0s, 1s, "media/camera.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/camera.mkv", videoProbe(1s)}},
        {{"marker", session, at(1s), "Overflow"}},
        timelineEndingAt(DurationNs{std::numeric_limits<std::int64_t>::max()}));

    const auto noProbe = planRecordingImport(missingProbe);
    const auto noRole = planRecordingImport(missingRole);
    const auto tooLarge = planRecordingImport(overflow);

    ASSERT_FALSE(noProbe.hasValue());
    EXPECT_EQ(noProbe.error().code(), ErrorCode::NotFound);
    ASSERT_FALSE(noRole.hasValue());
    EXPECT_EQ(noRole.error().code(), ErrorCode::NotFound);
    ASSERT_FALSE(tooLarge.hasValue());
    EXPECT_EQ(tooLarge.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingImportPlannerTest, RejectsMarkerPositionCollisionsAtomically) {
    const auto screen = source("screen");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", screen, StudioSourceRole::Screen,
                               true, transform(0.0, 0));
    auto duplicate = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/screen.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/screen.mkv", videoProbe(1s)}},
        {{"first", session, at(250ms), "First"},
         {"second", session, at(250ms), "Second"}});
    auto existingTimeline = timelineEndingAt(5s);
    ASSERT_TRUE(existingTimeline.addMarker(
                    creator::domain::TimelineMarker::create(
                        creator::domain::MarkerId::create("existing-marker")
                            .value(),
                        at(5250ms), "Existing")
                        .value())
                    .hasValue());
    auto existing = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/screen.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/screen.mkv", videoProbe(1s)}},
        {{"new-marker", session, at(250ms), "New"}},
        std::move(existingTimeline));

    const auto duplicateResult = planRecordingImport(duplicate);
    const auto existingResult = planRecordingImport(existing);

    ASSERT_FALSE(duplicateResult.hasValue());
    EXPECT_EQ(duplicateResult.error().code(), ErrorCode::AlreadyExists);
    ASSERT_FALSE(existingResult.hasValue());
    EXPECT_EQ(existingResult.error().code(), ErrorCode::AlreadyExists);
}

TEST(RecordingImportPlannerTest, RejectsInvalidFailedSegmentMetadata) {
    const auto screen = source("screen");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", screen, StudioSourceRole::Screen,
                               true, transform(0.0, 0));
    auto input = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, -1ns, "media/failed.mkv",
                 SegmentStatus::Failed)},
        {visible}, {{session, 0, visible.id(), at(0s)}}, {});

    const auto planned = planRecordingImport(input);

    ASSERT_FALSE(planned.hasValue());
    EXPECT_EQ(planned.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingImportPlannerTest, RejectsDuplicateScenesAndSourceRoleMismatch) {
    const auto camera = source("camera");
    const auto session = SessionId::create("세션-α").value();
    const auto first = scene("duplicate", camera, StudioSourceRole::Camera,
                             true, transform(0.5, 1));
    const auto second = scene("duplicate", camera, StudioSourceRole::Camera,
                              false, transform(0.2, 1));
    const auto wrongRole = scene("wrong-role", camera,
                                 StudioSourceRole::Screen, true,
                                 transform(0.0, 0));
    auto duplicated = request(
        camera, StudioSourceRole::Camera,
        {segment(camera, 0, 0s, 1s, "media/camera.mkv")},
        {first, second}, {{session, 0, first.id(), at(0s)}},
        {{"media/camera.mkv", videoProbe(1s)}});
    auto mismatched = request(
        camera, StudioSourceRole::Camera,
        {segment(camera, 0, 0s, 1s, "media/camera.mkv")}, {wrongRole},
        {{session, 0, wrongRole.id(), at(0s)}},
        {{"media/camera.mkv", videoProbe(1s)}});

    EXPECT_FALSE(planRecordingImport(duplicated).hasValue());
    EXPECT_FALSE(planRecordingImport(mismatched).hasValue());
}

TEST(RecordingImportPlannerTest, RejectsInvalidUtf8AndMalformedProbeIdentity) {
    const auto screen = source("screen");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", screen, StudioSourceRole::Screen,
                               true, transform(0.0, 0));
    auto invalidText = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/screen.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/screen.mkv", videoProbe(1s)}},
        {{std::string{"\xC3\x28", 2}, session, at(250ms), "Marker"}});
    auto badProbe = videoProbe(1s);
    badProbe.sha256 = "ABC";
    badProbe.formatName.clear();
    badProbe.codecName.clear();
    auto malformedProbe = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/screen.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/screen.mkv", std::move(badProbe)}});

    const auto textResult = planRecordingImport(invalidText);
    const auto probeResult = planRecordingImport(malformedProbe);

    ASSERT_FALSE(textResult.hasValue());
    EXPECT_EQ(textResult.error().code(), ErrorCode::InvalidArgument);
    ASSERT_FALSE(probeResult.hasValue());
    EXPECT_EQ(probeResult.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingImportPlannerTest, RejectsAssetIdReferencedByCurrentTimeline) {
    const auto screen = source("screen");
    const auto session = SessionId::create("세션-α").value();
    const auto visible = scene("visible", screen, StudioSourceRole::Screen,
                               true, transform(0.0, 0));
    auto input = request(
        screen, StudioSourceRole::Screen,
        {segment(screen, 0, 0s, 1s, "media/screen.mkv")}, {visible},
        {{session, 0, visible.id(), at(0s)}},
        {{"media/screen.mkv", videoProbe(1s)}}, {},
        timelineEndingAt(5s, "recording/세션-α/asset/screen/0"));

    const auto planned = planRecordingImport(input);

    ASSERT_FALSE(planned.hasValue());
    EXPECT_EQ(planned.error().code(), ErrorCode::AlreadyExists);
}

}  // namespace
