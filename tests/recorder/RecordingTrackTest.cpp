#include "recorder/RecordingTrack.h"

#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using creator::domain::SourceId;
using creator::recorder::RecordingTrack;
using creator::recorder::TrackMediaKind;
using creator::recorder::TrackRole;
using creator::recorder::relativeSegmentPath;
using creator::recorder::safeSourcePathComponent;
using creator::recorder::safeTakePathComponent;
using creator::recorder::temporarySegmentPath;

SourceId source(std::string value) {
    return SourceId::create(std::move(value)).value();
}

creator::domain::SessionId session(std::string value) {
    return creator::domain::SessionId::create(std::move(value)).value();
}

RecordingTrack track(std::string id, TrackRole role) {
    return RecordingTrack::create(source(std::move(id)), role).value();
}

TEST(RecordingTrackTest, DerivesMediaKindFromRole) {
    EXPECT_EQ(track("screen-1", TrackRole::Screen).mediaKind(),
              TrackMediaKind::Video);
    EXPECT_EQ(track("camera-1", TrackRole::Camera).mediaKind(),
              TrackMediaKind::Video);
    EXPECT_EQ(track("avatar-1", TrackRole::Avatar).mediaKind(),
              TrackMediaKind::Video);
    EXPECT_EQ(track("preview-1", TrackRole::CompositePreview).mediaKind(),
              TrackMediaKind::Video);
    EXPECT_EQ(track("mic-1", TrackRole::Microphone).mediaKind(),
              TrackMediaKind::Audio);
    EXPECT_EQ(track("system-1", TrackRole::SystemAudio).mediaKind(),
              TrackMediaKind::Audio);
}

TEST(RecordingTrackTest, EncodesEveryPathSeparatorDotSpaceAndUnicodeByte) {
    EXPECT_EQ(safeSourcePathComponent(source("screen-1")).value(), "screen-1");
    EXPECT_EQ(safeSourcePathComponent(source("../built in/마이크")).value(),
              "%2E%2E%2Fbuilt%20in%2F%EB%A7%88%EC%9D%B4%ED%81%AC");
}

TEST(RecordingTrackTest, RejectsAComponentThatWouldExceedPortableBound) {
    EXPECT_FALSE(safeSourcePathComponent(source(std::string(129, 'a'))).hasValue());
    EXPECT_FALSE(RecordingTrack::create(source(std::string(129, 'a')), TrackRole::Screen)
                     .hasValue());
}

TEST(RecordingTrackTest, BuildsIndependentRolePathsAndFixedWidthIndices) {
    const auto screen = track("screen-1", TrackRole::Screen);
    const auto camera = track("camera-1", TrackRole::Camera);
    const auto avatar = track("avatar-1", TrackRole::Avatar);
    const auto microphone = track("mic-1", TrackRole::Microphone);
    const auto system = track("system-1", TrackRole::SystemAudio);
    const auto preview = track("preview-1", TrackRole::CompositePreview);

    EXPECT_EQ(relativeSegmentPath(screen, "take-1", 0).generic_string(),
              "media/screen/screen-1/take-1/segment_000000.mkv");
    EXPECT_EQ(relativeSegmentPath(camera, "take-1", 42).generic_string(),
              "media/camera/camera-1/take-1/segment_000042.mkv");
    EXPECT_EQ(relativeSegmentPath(avatar, "take-1", 9).generic_string(),
              "media/avatar/avatar-1/take-1/segment_000009.mkv");
    EXPECT_EQ(relativeSegmentPath(microphone, "take-1", 7).generic_string(),
              "audio/microphone/mic-1/take-1/segment_000007.mka");
    EXPECT_EQ(relativeSegmentPath(system, "take-1", 1'000'000).generic_string(),
              "audio/system/system-1/take-1/segment_1000000.mka");
    EXPECT_EQ(relativeSegmentPath(preview, "take-1", 3).generic_string(),
              "media/preview/preview-1/take-1/segment_000003.mkv");
}

// Every take (recording session) owns its own segment directory, so a second
// take of the same source in the same project never collides with the first.
TEST(RecordingTrackTest, SeparatesTakesOfTheSameSourceIntoTheirOwnDirectories) {
    const auto screen = track("screen-1", TrackRole::Screen);
    EXPECT_NE(relativeSegmentPath(screen, "take-1", 0),
              relativeSegmentPath(screen, "take-2", 0));
    EXPECT_EQ(safeTakePathComponent(session("3f2a-uuid")).value(), "3f2a-uuid");
    EXPECT_EQ(safeTakePathComponent(session("../a b")).value(), "%2E%2E%2Fa%20b");
    EXPECT_FALSE(safeTakePathComponent(session(std::string(129, 'a'))).hasValue());
}

TEST(RecordingTrackTest, TemporaryPathMirrorsFinalPathBelowPackageTmp) {
    const auto screen = track("screen-1", TrackRole::Screen);
    EXPECT_EQ(temporarySegmentPath(screen, "take-1", 9).generic_string(),
              ".tmp/media/screen/screen-1/take-1/segment_000009.mkv.part");
}

}  // namespace
