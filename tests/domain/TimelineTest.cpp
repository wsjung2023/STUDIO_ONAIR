#include "domain/Timeline.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::AudioEnvelope;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;
using creator::domain::VisualTransform;

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{DurationNs{nanoseconds}};
}

AssetId makeAssetId(std::string value) {
    return AssetId::create(std::move(value)).value();
}

TrackId makeTrackId(std::string value) {
    return TrackId::create(std::move(value)).value();
}

ClipId makeClipId(std::string value) {
    return ClipId::create(std::move(value)).value();
}

MediaAsset videoAsset() {
    return MediaAsset::create(
               makeAssetId("screen-asset"), MediaKind::Video, "media/screen.mkv",
               DurationNs{100},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 1000, "screen-hash", AssetAvailability::Available)
        .value();
}

MediaAsset audioAsset() {
    return MediaAsset::create(
               makeAssetId("mic-asset"), MediaKind::Audio, "audio/mic.mka",
               DurationNs{100}, std::nullopt,
               AudioAssetMetadata{.sampleRate = 48'000, .channels = 1},
               1000, "mic-hash", AssetAvailability::Available)
        .value();
}

Clip videoClip(std::string id, std::int64_t sourceStart,
               std::int64_t timelineStart, std::int64_t duration) {
    auto created = Clip::createAsset(
        makeClipId(std::move(id)), videoAsset(),
        TimeRange::create(at(sourceStart), DurationNs{duration}).value(),
        TimeRange::create(at(timelineStart), DurationNs{duration}).value(),
        true,
        VisualTransform::create(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
                                0.0, 0.0, 0.0, 0.0, 1.0, 0)
            .value(),
        std::nullopt);
    EXPECT_TRUE(created.hasValue())
        << (created.hasValue() ? std::string{} : created.error().message());
    return std::move(created).value();
}

Timeline timeline() {
    return Timeline::create(TimelineId::create("main-timeline").value(), "Main",
                            FrameRate::create(60, 1).value())
        .value();
}

TEST(TimelineTest, AddsTrackAndKeepsClipOrder) {
    auto value = timeline();
    const auto trackId = makeTrackId("screen-track");
    ASSERT_TRUE(value.addTrack(
                         Track::create(trackId, TrackKind::Video, "Screen", true, false)
                             .value())
                    .hasValue());

    ASSERT_TRUE(value.insertClip(trackId, videoClip("later", 20, 20, 10)).hasValue());
    ASSERT_TRUE(value.insertClip(trackId, videoClip("earlier", 0, 0, 10)).hasValue());

    ASSERT_EQ(value.tracks().size(), 1U);
    ASSERT_EQ(value.tracks().front().clips().size(), 2U);
    EXPECT_EQ(value.tracks().front().clips()[0].id(), makeClipId("earlier"));
    EXPECT_EQ(value.tracks().front().clips()[1].id(), makeClipId("later"));
}

TEST(TimelineTest, AllowsTouchingClipsAndRejectsOverlapAtomically) {
    auto value = timeline();
    const auto trackId = makeTrackId("screen-track");
    ASSERT_TRUE(value.addTrack(
                         Track::create(trackId, TrackKind::Video, "Screen", true, false)
                             .value())
                    .hasValue());
    ASSERT_TRUE(value.insertClip(trackId, videoClip("first", 0, 0, 10)).hasValue());
    ASSERT_TRUE(value.insertClip(trackId, videoClip("touching", 10, 10, 10)).hasValue());
    const auto before = value;

    const auto overlap = value.insertClip(trackId, videoClip("overlap", 20, 9, 3));

    ASSERT_FALSE(overlap.hasValue());
    EXPECT_EQ(overlap.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(value, before);
}

TEST(TimelineTest, RejectsDuplicateTrackAndClipIdsAcrossTracks) {
    auto value = timeline();
    const auto firstTrack = makeTrackId("v1");
    const auto secondTrack = makeTrackId("v2");
    ASSERT_TRUE(value.addTrack(
                         Track::create(firstTrack, TrackKind::Video, "V1", true, false)
                             .value())
                    .hasValue());
    const auto duplicateTrack = value.addTrack(
        Track::create(firstTrack, TrackKind::Video, "Again", true, false).value());
    ASSERT_FALSE(duplicateTrack.hasValue());
    EXPECT_EQ(duplicateTrack.error().code(), ErrorCode::AlreadyExists);

    ASSERT_TRUE(value.addTrack(
                         Track::create(secondTrack, TrackKind::Video, "V2", true, false)
                             .value())
                    .hasValue());
    ASSERT_TRUE(value.insertClip(firstTrack, videoClip("same-id", 0, 0, 10)).hasValue());
    const auto duplicateClip =
        value.insertClip(secondTrack, videoClip("same-id", 20, 20, 10));
    ASSERT_FALSE(duplicateClip.hasValue());
    EXPECT_EQ(duplicateClip.error().code(), ErrorCode::AlreadyExists);
}

TEST(TimelineTest, RejectsClipOnIncompatibleTrack) {
    auto value = timeline();
    const auto audioTrack = makeTrackId("a1");
    ASSERT_TRUE(value.addTrack(
                         Track::create(audioTrack, TrackKind::Audio, "Microphone", true, false)
                             .value())
                    .hasValue());

    const auto result = value.insertClip(audioTrack, videoClip("screen", 0, 0, 10));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(TimelineTest, ValidatesClipAgainstAssetAndMatchingDurations) {
    const auto beyondAsset = Clip::createAsset(
        makeClipId("beyond"), videoAsset(),
        TimeRange::create(at(95), DurationNs{10}).value(),
        TimeRange::create(at(0), DurationNs{10}).value(), true, std::nullopt,
        std::nullopt);
    const auto mismatchedDuration = Clip::createAsset(
        makeClipId("speed"), videoAsset(),
        TimeRange::create(at(0), DurationNs{10}).value(),
        TimeRange::create(at(0), DurationNs{9}).value(), true, std::nullopt,
        std::nullopt);

    EXPECT_FALSE(beyondAsset.hasValue());
    EXPECT_FALSE(mismatchedDuration.hasValue());
}

TEST(TimelineTest, CreatesAudioClipWithEnvelopeAndRejectsVisualData) {
    const auto source = TimeRange::create(at(0), DurationNs{20}).value();
    const auto placed = TimeRange::create(at(10), DurationNs{20}).value();
    const auto envelope =
        AudioEnvelope::create(-3.0, DurationNs{2}, DurationNs{3}, DurationNs{20})
            .value();
    const auto valid = Clip::createAsset(makeClipId("mic"), audioAsset(), source,
                                         placed, true, std::nullopt, envelope);
    const auto visual = Clip::createAsset(
        makeClipId("wrong"), audioAsset(), source, placed, true,
        VisualTransform::create(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
                                0.0, 0.0, 0.0, 0.0, 1.0, 0)
            .value(),
        envelope);

    ASSERT_TRUE(valid.hasValue());
    EXPECT_TRUE(valid.value().audioEnvelope().has_value());
    EXPECT_FALSE(visual.hasValue());
}

TEST(TimelineTest, ReplacesAndRemovesClipByTypedIdentity) {
    auto value = timeline();
    const auto trackId = makeTrackId("v1");
    ASSERT_TRUE(value.addTrack(
                         Track::create(trackId, TrackKind::Video, "V1", true, false)
                             .value())
                    .hasValue());
    ASSERT_TRUE(value.insertClip(trackId, videoClip("clip", 0, 0, 10)).hasValue());
    const auto replacement = videoClip("clip", 10, 20, 10);

    ASSERT_TRUE(value.replaceClip(trackId, makeClipId("clip"), replacement).hasValue());
    EXPECT_EQ(value.track(trackId)->clips().front(), replacement);
    const auto removed = value.removeClip(trackId, makeClipId("clip"));
    ASSERT_TRUE(removed.hasValue());
    EXPECT_EQ(removed.value(), replacement);
    EXPECT_TRUE(value.track(trackId)->clips().empty());
}

}  // namespace
