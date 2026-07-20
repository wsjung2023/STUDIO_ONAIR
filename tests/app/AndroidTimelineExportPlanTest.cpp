#include "app/android/AndroidTimelineExportPlan.h"

#include "core/Uuid.h"
#include "domain/Timeline.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace creator;
namespace fs = std::filesystem;

class AndroidTimelineExportPlanTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("creator-studio-android-export-plan-" + core::generateUuidV4());
        fs::create_directories(root_ / "media");
        fs::create_directories(root_ / "cache/generated");
        touch(root_ / "media/source.mp4");
        touch(root_ / "cache/generated/title.png");
        touch(root_ / "cache/generated/caption.png");
    }

    void TearDown() override { fs::remove_all(root_); }

    static void touch(const fs::path& path) {
        std::ofstream output(path, std::ios::binary);
        output.put('x');
    }

    edit_engine::TimelineSnapshot snapshot(
        bool generatedOverlays = true,
        domain::AssetAvailability availability =
            domain::AssetAvailability::Available) const {
        const auto rate = core::FrameRate::create(30, 1).value();
        const auto duration = core::DurationNs{1'000'000'000};
        const auto range = domain::TimeRange::create(core::TimestampNs{},
                                                      duration)
                               .value();
        auto asset = domain::MediaAsset::create(
                         domain::AssetId::create("source").value(),
                         domain::MediaKind::Video, "media/source.mp4", duration,
                         domain::VideoAssetMetadata{1920, 1080, rate},
                         domain::AudioAssetMetadata{48'000, 2}, 1, "fingerprint",
                         availability)
                         .value();
        auto timeline = domain::Timeline::create(
                            domain::TimelineId::create("main").value(), "Main",
                            rate)
                            .value();
        const auto videoTrack = domain::TrackId::create("video").value();
        const auto titleTrack = domain::TrackId::create("title-track").value();
        const auto captionTrack =
            domain::TrackId::create("caption-track").value();
        EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                          videoTrack, domain::TrackKind::Video,
                                          "Video", true, false)
                                          .value())
                        .hasValue());
        EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                          titleTrack, domain::TrackKind::Title,
                                          "Title", true, false)
                                          .value())
                        .hasValue());
        EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                          captionTrack,
                                          domain::TrackKind::Caption, "Caption",
                                          true, false)
                                          .value())
                        .hasValue());
        const auto transform = domain::VisualTransform::create(
                                   0.6, 0.05, 0.35, 0.35, 1.0, 1.0, 5.0,
                                   0.1, 0.0, 0.0, 0.1, 0.9, 2)
                                   .value();
        const auto envelope = domain::AudioEnvelope::create(
                                  -3.0, core::DurationNs{100'000'000},
                                  core::DurationNs{100'000'000}, duration)
                                  .value();
        EXPECT_TRUE(timeline.insertClip(
                                videoTrack,
                                domain::Clip::createAsset(
                                    domain::ClipId::create("source-clip").value(),
                                    asset, range, range, true, transform, envelope)
                                    .value())
                        .hasValue());
        const auto titleId = domain::ClipId::create("title").value();
        const auto title = domain::TitlePayload::create(
                               "Title", "Creator Sans", 0.5, 0.5,
                               domain::RgbaColor::parse("#ffffffff").value(),
                               domain::RgbaColor::parse("#00000000").value(),
                               domain::TextAlignment::Center)
                               .value();
        EXPECT_TRUE(timeline.insertClip(
                                titleTrack,
                                domain::Clip::createTitle(titleId, range, true,
                                                          title, std::nullopt)
                                    .value())
                        .hasValue());
        const auto cue = domain::CaptionCue::create(
                             domain::CueId::create("cue").value(),
                             core::DurationNs{200'000'000},
                             core::DurationNs{300'000'000}, "Caption")
                             .value();
        const auto captionId = domain::ClipId::create("caption").value();
        EXPECT_TRUE(timeline.insertClip(
                                captionTrack,
                                domain::Clip::createCaption(captionId, range,
                                                            true, {cue},
                                                            std::nullopt)
                                    .value())
                        .hasValue());

        std::vector<edit_engine::GeneratedOverlayDescriptor> overlays;
        if (generatedOverlays) {
            overlays.push_back(
                edit_engine::GeneratedOverlayDescriptor::create(
                    titleId, std::nullopt, "cache/generated/title.png", range,
                    "Creator Sans")
                    .value());
            overlays.push_back(
                edit_engine::GeneratedOverlayDescriptor::create(
                    captionId, cue.id(), "cache/generated/caption.png",
                    domain::TimeRange::create(
                        core::TimestampNs{core::DurationNs{200'000'000}},
                        core::DurationNs{300'000'000})
                        .value(),
                    "Creator Sans")
                    .value());
        }
        return {.timeline = std::move(timeline),
                .revision = domain::TimelineRevision::create(1).value(),
                .assets = {asset},
                .mediaRoot = root_,
                .canvasWidth = 1920,
                .canvasHeight = 1080,
                .generatedOverlays = std::move(overlays)};
    }

    edit_engine::RenderRequest request(edit_engine::TimelineSnapshot value) const {
        return edit_engine::RenderRequest::create(
                   domain::ProjectId::create("project").value(),
                   std::move(value), root_ / "export.mp4",
                   edit_engine::RenderPreset::h2641080p30().value(),
                   edit_engine::RenderOverwritePolicy::FailIfExists)
            .value();
    }

    fs::path root_;
};

TEST_F(AndroidTimelineExportPlanTest,
       FreezesVideoAudioTitleAndCaptionWithoutSilentOmission) {
    auto planned = app::android::buildAndroidTimelineExportPlan(
        request(snapshot()), root_ / ".partial.mp4");
    ASSERT_TRUE(planned.hasValue()) << planned.error().message();
    EXPECT_EQ(planned.value().visualClipCount, 3U);
    EXPECT_EQ(planned.value().audioClipCount, 1U);
    EXPECT_EQ(planned.value().duration, core::DurationNs{1'000'000'000});
    const auto document = QJsonDocument::fromJson(planned.value().json);
    ASSERT_TRUE(document.isObject());
    const auto root = document.object();
    EXPECT_EQ(root.value(QStringLiteral("visualClips")).toArray().size(), 3);
    EXPECT_EQ(root.value(QStringLiteral("audioClips")).toArray().size(), 1);
    EXPECT_EQ(root.value(QStringLiteral("durationUs")).toInteger(), 1'000'000);
    const auto visual = root.value(QStringLiteral("visualClips"))
                            .toArray()
                            .first()
                            .toObject();
    EXPECT_EQ(visual.value(QStringLiteral("kind")).toString(),
              QStringLiteral("video"));
    EXPECT_DOUBLE_EQ(visual.value(QStringLiteral("transform"))
                         .toObject()
                         .value(QStringLiteral("opacity"))
                         .toDouble(),
                     0.9);
}

TEST_F(AndroidTimelineExportPlanTest,
       RejectsMissingGeneratedRasterInsteadOfDroppingText) {
    auto planned = app::android::buildAndroidTimelineExportPlan(
        request(snapshot(false)), root_ / ".partial.mp4");
    ASSERT_FALSE(planned.hasValue());
    EXPECT_EQ(planned.error().code(), core::ErrorCode::InvalidState);
    EXPECT_NE(planned.error().message().find("overlay"), std::string::npos);
}

TEST_F(AndroidTimelineExportPlanTest, RejectsOfflineMediaInsteadOfExportingBlack) {
    auto planned = app::android::buildAndroidTimelineExportPlan(
        request(snapshot(true, domain::AssetAvailability::Offline)),
        root_ / ".partial.mp4");
    ASSERT_FALSE(planned.hasValue());
    EXPECT_NE(planned.error().message().find("offline"), std::string::npos);
}

}  // namespace
