#include "app/MediaBinModel.h"
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
using creator::app::TimelineTrackModel;
using creator::core::DurationNs;
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

    ASSERT_EQ(model.rowCount(), 1);
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

}  // namespace
