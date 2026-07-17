#include "app/MediaBinModel.h"
#include "app/StudioSceneModel.h"
#include "app/StudioSourceModel.h"
#include "app/TimelineTrackModel.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineTypes.h"

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using creator::app::MediaBinModel;
using creator::app::StudioSceneModel;
using creator::app::StudioSourceModel;
using creator::app::TimelineTrackModel;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::AudioEnvelope;
using creator::domain::CaptionCue;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CueId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TitlePayload;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;
using creator::domain::VisualTransform;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;

MediaAsset videoAsset(std::string id, std::string path,
                      AssetAvailability availability) {
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Video, path,
               DurationNs{5'000'000'000},
               VideoAssetMetadata{1920, 1080, FrameRate::create(60, 1).value()},
               AudioAssetMetadata{48'000, 2}, 42'000, "fingerprint",
               availability)
        .value();
}

TEST(MediaBinModelTest, ExposesStableUnicodeAndOfflineAssetRoles) {
    MediaBinModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    model.setAssets({videoAsset("screen-main", "media/화면 녹화.mp4",
                                AssetAvailability::Offline)});

    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(resetSpy.count(), 1);
    EXPECT_EQ(model.roleNames().value(MediaBinModel::AssetIdRole), "assetId");
    EXPECT_EQ(model.roleNames().value(MediaBinModel::PackagePathRole),
              "packagePath");
    const QModelIndex index = model.index(0, 0);
    EXPECT_EQ(model.data(index, MediaBinModel::AssetIdRole).toString(),
              QStringLiteral("screen-main"));
    EXPECT_EQ(model.data(index, MediaBinModel::PackagePathRole).toString(),
              QString::fromUtf8("media/화면 녹화.mp4"));
    EXPECT_EQ(model.data(index, MediaBinModel::KindRole).toString(),
              QStringLiteral("video"));
    EXPECT_EQ(model.data(index, MediaBinModel::DurationNsRole).toLongLong(),
              5'000'000'000LL);
    EXPECT_FALSE(model.data(index, MediaBinModel::AvailableRole).toBool());

    const QVariantMap video =
        model.data(index, MediaBinModel::VideoMetadataRole).toMap();
    EXPECT_EQ(video.value(QStringLiteral("width")).toInt(), 1920);
    EXPECT_EQ(video.value(QStringLiteral("frameRateNumerator")).toInt(), 60);
    const QVariantMap audio =
        model.data(index, MediaBinModel::AudioMetadataRole).toMap();
    EXPECT_EQ(audio.value(QStringLiteral("sampleRate")).toInt(), 48'000);
    EXPECT_EQ(audio.value(QStringLiteral("channels")).toInt(), 2);
}

TEST(MediaBinModelTest, ReplacesRowsWithExactlyOneReset) {
    MediaBinModel model;
    model.setAssets({videoAsset("one", "media/one.mp4",
                                AssetAvailability::Available)});
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    model.setAssets({videoAsset("two", "media/two.mp4",
                                AssetAvailability::Available),
                     videoAsset("three", "media/three.mp4",
                                AssetAvailability::Available)});
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(resetSpy.count(), 1);
    EXPECT_EQ(model.data(model.index(1, 0), MediaBinModel::AssetIdRole).toString(),
              QStringLiteral("three"));
}

TEST(TimelineTrackModelTest, ExposesTracksAndCompleteClipDtosInStableOrder) {
    const MediaAsset asset = videoAsset("screen", "media/screen.mp4",
                                        AssetAvailability::Available);
    auto timeline = Timeline::create(TimelineId::create("main").value(),
                                     "강의 편집", FrameRate::create(60, 1).value())
                        .value();
    ASSERT_TRUE(timeline.addTrack(Track::create(TrackId::create("v1").value(),
                                                TrackKind::Video, "화면", true,
                                                false)
                                      .value())
                    .hasValue());
    ASSERT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("title-1").value(),
                                          TrackKind::Title, "Titles", true,
                                          false)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("caption-1").value(),
                                          TrackKind::Caption, "Captions", true,
                                          false)
                                .value())
                    .hasValue());
    const auto generatedRange =
        TimeRange::create(TimestampNs{DurationNs{200}}, DurationNs{800}).value();
    const auto titlePayload = TitlePayload::create(
        "Course title", "Noto Sans", 0.25, 0.10,
        RgbaColor::parse("#ffffffff").value(),
        RgbaColor::parse("#00000080").value(), TextAlignment::Center)
                                  .value();
    ASSERT_TRUE(timeline.insertClip(
                            TrackId::create("title-1").value(),
                            Clip::createTitle(ClipId::create("title-a").value(),
                                              generatedRange, true, titlePayload,
                                              std::nullopt)
                                .value())
                    .hasValue());
    const auto cue = CaptionCue::create(
                         CueId::create("cue-a").value(), DurationNs{50},
                         DurationNs{300}, "First caption")
                         .value();
    ASSERT_TRUE(timeline.insertClip(
                            TrackId::create("caption-1").value(),
                            Clip::createCaption(
                                ClipId::create("caption-a").value(),
                                generatedRange, true, {cue}, std::nullopt)
                                .value())
                    .hasValue());
    const auto sourceRange =
        TimeRange::create(TimestampNs{DurationNs{10}}, DurationNs{1'000}).value();
    const auto timelineRange =
        TimeRange::create(TimestampNs{DurationNs{20}}, DurationNs{1'000}).value();
    const auto transform = VisualTransform::create(
                               0.1, 0.2, 0.8, 0.9, 1.0, 1.0, 3.0, 0.1,
                               0.2, 0.3, 0.4, 0.8, 7)
                               .value();
    const auto envelope =
        AudioEnvelope::create(-3.0, DurationNs{100}, DurationNs{200},
                              sourceRange.duration())
            .value();
    ASSERT_TRUE(timeline.insertClip(
                            TrackId::create("v1").value(),
                            Clip::createAsset(ClipId::create("clip-1").value(),
                                              asset, sourceRange, timelineRange,
                                              true, transform, envelope)
                                .value())
                    .hasValue());

    TimelineTrackModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    model.setTimeline(timeline);

    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(resetSpy.count(), 1);
    EXPECT_EQ(model.roleNames().value(TimelineTrackModel::TrackIdRole),
              "trackId");
    EXPECT_EQ(model.roleNames().value(TimelineTrackModel::ClipsRole), "clips");
    const QModelIndex index = model.index(0, 0);
    EXPECT_EQ(model.data(index, TimelineTrackModel::TrackIdRole).toString(),
              QStringLiteral("v1"));
    EXPECT_EQ(model.data(index, TimelineTrackModel::NameRole).toString(),
              QString::fromUtf8("화면"));
    EXPECT_EQ(model.data(index, TimelineTrackModel::KindRole).toString(),
              QStringLiteral("video"));
    EXPECT_TRUE(model.data(index, TimelineTrackModel::EnabledRole).toBool());
    EXPECT_FALSE(model.data(index, TimelineTrackModel::LockedRole).toBool());

    const QVariantList clips =
        model.data(index, TimelineTrackModel::ClipsRole).toList();
    ASSERT_EQ(clips.size(), 1);
    const QVariantMap clip = clips.front().toMap();
    EXPECT_EQ(clip.value(QStringLiteral("id")).toString(),
              QStringLiteral("clip-1"));
    EXPECT_EQ(clip.value(QStringLiteral("assetId")).toString(),
              QStringLiteral("screen"));
    EXPECT_EQ(clip.value(QStringLiteral("mediaKind")).toString(),
              QStringLiteral("video"));
    EXPECT_EQ(clip.value(QStringLiteral("clipKind")).toString(),
              QStringLiteral("asset"));
    EXPECT_TRUE(clip.value(QStringLiteral("visualCompatible")).toBool());
    EXPECT_TRUE(clip.value(QStringLiteral("audioCompatible")).toBool());
    EXPECT_EQ(clip.value(QStringLiteral("sourceStartNs")).toLongLong(), 10);
    EXPECT_EQ(clip.value(QStringLiteral("sourceDurationNs")).toLongLong(), 1'000);
    EXPECT_EQ(clip.value(QStringLiteral("timelineStartNs")).toLongLong(), 20);
    EXPECT_EQ(clip.value(QStringLiteral("timelineDurationNs")).toLongLong(),
              1'000);
    EXPECT_EQ(clip.value(QStringLiteral("durationNs")).toLongLong(), 1'000);
    EXPECT_DOUBLE_EQ(clip.value(QStringLiteral("opacity")).toDouble(), 0.8);
    EXPECT_EQ(clip.value(QStringLiteral("zOrder")).toInt(), 7);
    EXPECT_DOUBLE_EQ(clip.value(QStringLiteral("gainDb")).toDouble(), -3.0);
    EXPECT_EQ(clip.value(QStringLiteral("fadeOutNs")).toLongLong(), 200);

    const QVariantMap title =
        model.data(model.index(1, 0), TimelineTrackModel::ClipsRole)
            .toList()
            .front()
            .toMap();
    EXPECT_EQ(title.value(QStringLiteral("clipKind")).toString(),
              QStringLiteral("title"));
    EXPECT_TRUE(title.value(QStringLiteral("visualCompatible")).toBool());
    EXPECT_FALSE(title.value(QStringLiteral("audioCompatible")).toBool());
    EXPECT_EQ(title.value(QStringLiteral("titleText")).toString(),
              QStringLiteral("Course title"));
    EXPECT_EQ(title.value(QStringLiteral("titleFontFamily")).toString(),
              QStringLiteral("Noto Sans"));
    EXPECT_EQ(title.value(QStringLiteral("titleForeground")).toString(),
              QStringLiteral("#ffffffff"));
    EXPECT_EQ(title.value(QStringLiteral("titleAlignment")).toString(),
              QStringLiteral("center"));

    const QVariantMap caption =
        model.data(model.index(2, 0), TimelineTrackModel::ClipsRole)
            .toList()
            .front()
            .toMap();
    EXPECT_EQ(caption.value(QStringLiteral("clipKind")).toString(),
              QStringLiteral("caption"));
    const QVariantList cues =
        caption.value(QStringLiteral("captionCues")).toList();
    ASSERT_EQ(cues.size(), 1);
    EXPECT_EQ(cues.front().toMap().value(QStringLiteral("cueId")).toString(),
              QStringLiteral("cue-a"));
    EXPECT_EQ(cues.front()
                  .toMap()
                  .value(QStringLiteral("startOffsetNs"))
                  .toLongLong(),
              50);
    EXPECT_EQ(cues.front().toMap().value(QStringLiteral("text")).toString(),
              QStringLiteral("First caption"));
}

TEST(TimelineTrackModelTest, ReplacesTimelineWithExactlyOneReset) {
    TimelineTrackModel model;
    auto timeline = Timeline::create(TimelineId::create("empty").value(),
                                     "Empty", FrameRate::create(30, 1).value())
                        .value();
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    model.setTimeline(timeline);
    EXPECT_EQ(resetSpy.count(), 1);
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(StudioSceneModelTest, PublishesStableSceneRolesAndSelection) {
    auto scenes = creator::domain::defaultStudioScenes().value();
    StudioSceneModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    model.setScenes(scenes, scenes[1].id(), scenes[2].id());

    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(resetSpy.count(), 1);
    EXPECT_EQ(model.roleNames().value(StudioSceneModel::SceneIdRole),
              "sceneId");
    EXPECT_EQ(model.roleNames().value(StudioSceneModel::SourceCountRole),
              "sourceCount");
    const auto screen = model.index(1, 0);
    EXPECT_EQ(model.data(screen, StudioSceneModel::NameRole).toString(),
              QStringLiteral("Screen"));
    EXPECT_TRUE(model.data(screen, StudioSceneModel::ActiveRole).toBool());
    EXPECT_FALSE(model.data(screen, StudioSceneModel::SelectedRole).toBool());
    EXPECT_EQ(model.data(screen, StudioSceneModel::SourceCountRole).toInt(), 4);
    EXPECT_TRUE(model.data(model.index(2, 0),
                           StudioSceneModel::SelectedRole)
                    .toBool());
    resetSpy.clear();
    model.setScenes(scenes, scenes[1].id(), scenes[2].id());
    EXPECT_EQ(resetSpy.count(), 0);
}

TEST(StudioSourceModelTest, PublishesExactSourceTransformAndRoleNames) {
    const auto scene = creator::domain::defaultStudioScenes().value().front();
    StudioSourceModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    model.setScene(scene, scene.sources()[1].id());

    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(resetSpy.count(), 1);
    EXPECT_EQ(model.roleNames().value(StudioSourceModel::SourceIdRole),
              "sourceId");
    EXPECT_EQ(model.roleNames().value(StudioSourceModel::TransformRole),
              "transform");
    const auto camera = model.index(1, 0);
    EXPECT_EQ(model.data(camera, StudioSourceModel::RoleNameRole).toString(),
              QStringLiteral("camera"));
    EXPECT_TRUE(model.data(camera, StudioSourceModel::EnabledRole).toBool());
    EXPECT_TRUE(model.data(camera, StudioSourceModel::SelectedRole).toBool());
    const auto transform =
        model.data(camera, StudioSourceModel::TransformRole).toMap();
    EXPECT_DOUBLE_EQ(transform.value(QStringLiteral("x")).toDouble(), 0.70);
    EXPECT_DOUBLE_EQ(transform.value(QStringLiteral("width")).toDouble(),
                     0.25);
    EXPECT_EQ(transform.value(QStringLiteral("zOrder")).toInt(), 10);
    EXPECT_FALSE(model.data(model.index(2, 0),
                            StudioSourceModel::HasTransformRole)
                     .toBool());
    resetSpy.clear();
    model.setScene(scene, scene.sources()[1].id());
    EXPECT_EQ(resetSpy.count(), 0);
}

}  // namespace
