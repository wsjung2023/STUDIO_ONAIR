#include "domain/ImportRecordingCommand.h"

#include "core/Timebase.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"

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
using creator::domain::AudioAssetMetadata;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::ImportRecordingCommand;
using creator::domain::MarkerId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineMarker;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

CommandId commandId(std::string value = "import-recording") {
    return CommandId::create(std::move(value)).value();
}

TrackId trackId(std::string value) {
    return TrackId::create(std::move(value)).value();
}

ClipId clipId(std::string value) {
    return ClipId::create(std::move(value)).value();
}

Timeline emptyTimeline() {
    return Timeline::create(TimelineId::create("timeline").value(), "Main",
                            FrameRate::create(60, 1).value())
        .value();
}

MediaAsset asset(std::string id, MediaKind kind) {
    const bool video = kind == MediaKind::Video;
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), kind,
               video ? "media/video.mkv" : "audio/audio.mka", DurationNs{1'000},
               video ? std::optional{VideoAssetMetadata{
                           .width = 1920,
                           .height = 1080,
                           .frameRate = FrameRate::create(60, 1).value()}}
                     : std::nullopt,
               video ? std::nullopt
                     : std::optional{AudioAssetMetadata{
                           .sampleRate = 48'000, .channels = 2}},
               4'096, video ? "video-hash" : "audio-hash",
               AssetAvailability::Available)
        .value();
}

Track populatedTrack(std::string trackValue, TrackKind trackKind,
                     std::string clipValue, std::string assetValue,
                     MediaKind mediaKind, std::int64_t start) {
    auto staging = emptyTimeline();
    const auto id = trackId(std::move(trackValue));
    EXPECT_TRUE(staging.addTrack(
                           Track::create(id, trackKind, "Recorded source", true, false)
                               .value())
                    .hasValue());
    const auto range = TimeRange::create(at(start), DurationNs{100}).value();
    EXPECT_TRUE(staging.insertClip(
                           id, Clip::createAsset(
                                   clipId(std::move(clipValue)),
                                   asset(std::move(assetValue), mediaKind),
                                   TimeRange::create(at(0), DurationNs{100}).value(),
                                   range, true, std::nullopt, std::nullopt)
                                   .value())
                    .hasValue());
    return staging.tracks().front();
}

std::vector<Track> recordedTracks() {
    return {
        populatedTrack("screen-track", TrackKind::Video, "screen-clip",
                       "screen-asset", MediaKind::Video, 0),
        populatedTrack("camera-track", TrackKind::Video, "camera-clip",
                       "camera-asset", MediaKind::Video, 0),
        populatedTrack("system-audio-track", TrackKind::Audio,
                       "system-audio-clip", "system-audio-asset",
                       MediaKind::Audio, 0),
        populatedTrack("microphone-track", TrackKind::Audio,
                       "microphone-clip", "microphone-asset",
                       MediaKind::Audio, 0),
    };
}

TimelineMarker marker(std::string id, std::int64_t position,
                      std::string label) {
    return TimelineMarker::create(MarkerId::create(std::move(id)).value(),
                                  at(position), std::move(label))
        .value();
}

std::vector<TimelineMarker> recordingMarkers() {
    return {marker("recording-start", 0, "녹화 시작"),
            marker("scene-change", 100, "장면 전환"),
            marker("chapter", 200, "챕터 1"),
            marker("pause", 300, "일시 정지"),
            marker("recording-end", 400, "녹화 종료")};
}

std::string invalidUtf8(std::string prefix) {
    prefix.push_back(static_cast<char>(0x80));
    return prefix;
}

TEST(ImportRecordingCommandTest, ExecuteUndoRedoRestoresExactTimeline) {
    auto value = emptyTimeline();
    const auto before = value;
    auto created = ImportRecordingCommand::create(
        commandId(), recordedTracks(), recordingMarkers());
    ASSERT_TRUE(created.hasValue());
    auto command = std::move(created).value();

    ASSERT_TRUE(command->execute(value).hasValue());
    const auto imported = value;
    EXPECT_EQ(value.tracks().size(), 4U);
    EXPECT_EQ(value.markers().size(), 5U);

    ASSERT_TRUE(command->undo(value).hasValue());
    EXPECT_EQ(value, before);
    ASSERT_TRUE(command->execute(value).hasValue());
    EXPECT_EQ(value, imported);
}

TEST(ImportRecordingCommandTest, RejectsImportedIdentityDuplicatesAtCreation) {
    const auto tracks = recordedTracks();
    const auto markers = recordingMarkers();

    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("duplicate-track"), {tracks[0], tracks[0]}, markers)
                     .hasValue());

    const auto duplicateClip = populatedTrack(
        "other-video-track", TrackKind::Video, "screen-clip", "other-asset",
        MediaKind::Video, 200);
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("duplicate-clip"), {tracks[0], duplicateClip}, markers)
                     .hasValue());

    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("duplicate-marker-id"), tracks,
                     {markers[0], marker("recording-start", 999, "duplicate")})
                     .hasValue());
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("duplicate-marker-position"), tracks,
                     {markers[0], marker("other-marker", 0, "duplicate")})
                     .hasValue());
}

TEST(ImportRecordingCommandTest, RejectsStringsThatWouldCorruptCommandJson) {
    const auto invalidName = Track::create(
        trackId("invalid-name-track"), TrackKind::Video,
        invalidUtf8("name"), true, false).value();
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("invalid-name"), {invalidName}, {})
                     .hasValue());

    const auto invalidTrackId = Track::create(
        trackId(invalidUtf8("track")), TrackKind::Video,
        "Track", true, false).value();
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("invalid-track-id"), {invalidTrackId}, {})
                     .hasValue());

    const auto invalidClipId = populatedTrack(
        "clip-id-track", TrackKind::Video, invalidUtf8("clip"), "asset",
        MediaKind::Video, 0);
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("invalid-clip-id"), {invalidClipId}, {})
                     .hasValue());

    const auto invalidAssetId = populatedTrack(
        "asset-id-track", TrackKind::Video, "clip", invalidUtf8("asset"),
        MediaKind::Video, 0);
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("invalid-asset-id"), {invalidAssetId}, {})
                     .hasValue());

    const auto invalidMarker = TimelineMarker::create(
        MarkerId::create(invalidUtf8("marker")).value(), at(0), "Marker")
                                   .value();
    EXPECT_FALSE(ImportRecordingCommand::create(
                     commandId("invalid-marker-id"), {}, {invalidMarker})
                     .hasValue());
}

TEST(ImportRecordingCommandTest, RejectsTimelineCollisionsBeforeAnyMutation) {
    const auto tracks = recordedTracks();
    const auto markers = recordingMarkers();

    auto trackCollision = emptyTimeline();
    ASSERT_TRUE(trackCollision.addTrack(
                                  Track::create(tracks[0].id(), TrackKind::Video,
                                                "Existing", true, false)
                                      .value())
                    .hasValue());
    const auto trackBefore = trackCollision;
    auto trackCommand = ImportRecordingCommand::create(
        commandId("track-collision"), tracks, markers).value();
    EXPECT_FALSE(trackCommand->execute(trackCollision).hasValue());
    EXPECT_EQ(trackCollision, trackBefore);

    auto clipCollision = emptyTimeline();
    const auto existing = populatedTrack(
        "existing-track", TrackKind::Video, "screen-clip", "existing-asset",
        MediaKind::Video, 500);
    ASSERT_TRUE(clipCollision.addTrack(existing).hasValue());
    const auto clipBefore = clipCollision;
    auto clipCommand = ImportRecordingCommand::create(
        commandId("clip-collision"), tracks, markers).value();
    EXPECT_FALSE(clipCommand->execute(clipCollision).hasValue());
    EXPECT_EQ(clipCollision, clipBefore);

    for (const auto conflictingMarker : {
             marker("recording-start", 999, "same id"),
             marker("existing-marker", 0, "same position")}) {
        auto markerCollision = emptyTimeline();
        ASSERT_TRUE(markerCollision.addMarker(conflictingMarker).hasValue());
        const auto markerBefore = markerCollision;
        auto markerCommand = ImportRecordingCommand::create(
            commandId("marker-collision"), tracks, markers).value();
        EXPECT_FALSE(markerCommand->execute(markerCollision).hasValue());
        EXPECT_EQ(markerCollision, markerBefore);
    }
}

TEST(ImportRecordingCommandTest, RecordIsVersionedDeterministicAndUtf8Safe) {
    auto created = ImportRecordingCommand::create(
        commandId(), recordedTracks(), recordingMarkers());
    ASSERT_TRUE(created.hasValue());

    const auto record = created.value()->record();

    EXPECT_EQ(record.type, "IMPORT_RECORDING");
    EXPECT_NE(record.payload.find("\"version\":1"), std::string::npos);
    EXPECT_NE(record.payload.find("\"trackId\":\"screen-track\""),
              std::string::npos);
    EXPECT_NE(record.payload.find("녹화 시작"), std::string::npos);
    EXPECT_EQ(record.payload.find('\n'), std::string::npos);
}

}  // namespace
