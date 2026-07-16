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
using creator::recorder::temporarySegmentPath;

SourceId source(std::string value) {
    return SourceId::create(std::move(value)).value();
}

RecordingTrack track(std::string id, TrackRole role) {
    return RecordingTrack::create(source(std::move(id)), role).value();
}

TEST(RecordingTrackTest, DerivesMediaKindFromRole) {
    EXPECT_EQ(track("screen-1", TrackRole::Screen).mediaKind(),
              TrackMediaKind::Video);
    EXPECT_EQ(track("camera-1", TrackRole::Camera).mediaKind(),
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
    const auto microphone = track("mic-1", TrackRole::Microphone);
    const auto system = track("system-1", TrackRole::SystemAudio);
    const auto preview = track("preview-1", TrackRole::CompositePreview);

    EXPECT_EQ(relativeSegmentPath(screen, 0).generic_string(),
              "media/screen/screen-1/segment_000000.mkv");
    EXPECT_EQ(relativeSegmentPath(camera, 42).generic_string(),
              "media/camera/camera-1/segment_000042.mkv");
    EXPECT_EQ(relativeSegmentPath(microphone, 7).generic_string(),
              "audio/microphone/mic-1/segment_000007.mka");
    EXPECT_EQ(relativeSegmentPath(system, 1'000'000).generic_string(),
              "audio/system/system-1/segment_1000000.mka");
    EXPECT_EQ(relativeSegmentPath(preview, 3).generic_string(),
              "media/preview/preview-1/segment_000003.mkv");
}

TEST(RecordingTrackTest, TemporaryPathMirrorsFinalPathBelowPackageTmp) {
    const auto screen = track("screen-1", TrackRole::Screen);
    EXPECT_EQ(temporarySegmentPath(screen, 9).generic_string(),
              ".tmp/media/screen/screen-1/segment_000009.mkv.part");
}

}  // namespace
