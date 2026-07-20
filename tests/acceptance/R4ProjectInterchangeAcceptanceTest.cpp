#include "core/Timebase.h"
#include "core/Utc.h"
#include "core/Uuid.h"
#include "domain/MediaAsset.h"
#include "domain/SplitClipCommand.h"
#include "domain/StudioScene.h"
#include "domain/Timeline.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::CaptionCue;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::CueId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::ProjectId;
using creator::domain::RgbaColor;
using creator::domain::SceneId;
using creator::domain::SceneSource;
using creator::domain::SourceId;
using creator::domain::SplitClipCommand;
using creator::domain::StudioScene;
using creator::domain::StudioSourceRole;
using creator::domain::TextAlignment;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::TitlePayload;
using creator::domain::VideoAssetMetadata;
using creator::domain::VisualTransform;
using creator::project_store::EditEventKind;
using creator::project_store::EditEventRecord;
using creator::project_store::ProjectPackageStore;
using creator::project_store::SqliteStudioStore;
using creator::project_store::SqliteTimelineStore;
using creator::project_store::StudioSnapshot;
using creator::project_store::TimelineCommit;

constexpr std::int64_t kSecond = 1'000'000'000;

std::string utf8(std::u8string_view value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

MediaAsset mobileVideo() {
    return MediaAsset::create(
               AssetId::create("mobile-video").value(), MediaKind::Video,
               utf8(u8"media/강의 화면.mp4"), DurationNs{8 * kSecond},
               VideoAssetMetadata{
                   .width = 1920,
                   .height = 1080,
                   .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 1'024, "portable-video-fingerprint",
               AssetAvailability::Available)
        .value();
}

Timeline mobileTimeline(const MediaAsset& video) {
    auto timeline =
        Timeline::create(TimelineId::create("main").value(),
                         utf8(u8"모바일 편집 타임라인"),
                         FrameRate::create(60, 1).value())
            .value();
    const auto videoTrack = TrackId::create("video").value();
    const auto titleTrack = TrackId::create("title").value();
    const auto captionTrack = TrackId::create("caption").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(videoTrack, TrackKind::Video,
                                          utf8(u8"화면"), true, false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(titleTrack, TrackKind::Title,
                                          utf8(u8"제목"), true, false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(captionTrack, TrackKind::Caption,
                                          utf8(u8"자막"), true, false)
                                .value())
                    .hasValue());

    const auto fullRange =
        TimeRange::create(at(0), DurationNs{8 * kSecond}).value();
    EXPECT_TRUE(timeline.insertClip(
                            videoTrack,
                            Clip::createAsset(
                                ClipId::create("video-clip").value(), video,
                                fullRange, fullRange, true, std::nullopt,
                                std::nullopt)
                                .value())
                    .hasValue());

    const auto foreground = RgbaColor::parse("#ffffffff").value();
    const auto background = RgbaColor::parse("#00000080").value();
    EXPECT_TRUE(timeline.insertClip(
                            titleTrack,
                            Clip::createTitle(
                                ClipId::create("title-clip").value(),
                                TimeRange::create(at(kSecond),
                                                  DurationNs{2 * kSecond})
                                    .value(),
                                true,
                                TitlePayload::create(
                                    utf8(u8"플랫폼 중립 제목"), "Noto Sans CJK KR",
                                    0.5, 0.88, foreground, background,
                                    TextAlignment::Center)
                                    .value(),
                                std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            captionTrack,
                            Clip::createCaption(
                                ClipId::create("caption-clip").value(),
                                TimeRange::create(at(0),
                                                  DurationNs{4 * kSecond})
                                    .value(),
                                true,
                                {CaptionCue::create(
                                     CueId::create("cue-1").value(),
                                     DurationNs{0}, DurationNs{2 * kSecond},
                                     utf8(u8"안드로이드에서 작성"))
                                     .value(),
                                 CaptionCue::create(
                                     CueId::create("cue-2").value(),
                                     DurationNs{2 * kSecond},
                                     DurationNs{2 * kSecond},
                                     utf8(u8"데스크톱에서 다시 열기"))
                                     .value()},
                                std::nullopt)
                                .value())
                    .hasValue());
    return timeline;
}

StudioScene mobileScene() {
    auto source = SceneSource::create(
        SourceId::create("screen").value(), StudioSourceRole::Screen,
        utf8(u8"공유 화면"), 0, true,
        VisualTransform::create(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0,
                                0.0, 0.0, 0.0, 1.0, 0)
            .value());
    return StudioScene::create(SceneId::create("scene-main").value(),
                               utf8(u8"모바일 장면"), 0,
                               {std::move(source).value()})
        .value();
}

class TemporaryProjectRoot final {
public:
    TemporaryProjectRoot()
        : path_(fs::temp_directory_path() /
                ("creator-studio-r4-interchange-" +
                 creator::core::generateUuidV4())) {
        fs::create_directories(path_);
    }

    ~TemporaryProjectRoot() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string readText(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

TEST(R4ProjectInterchangeAcceptance, RoundTripsMobilePackageThroughDesktopEdit) {
    TemporaryProjectRoot root;
    const auto mobileRoot = root.path() / fs::path{u8"Android-모바일"};
    const auto desktopRoot = root.path() / fs::path{u8"Desktop-데스크톱"};
    ASSERT_TRUE(fs::create_directories(mobileRoot));
    ASSERT_TRUE(fs::create_directories(desktopRoot));
    const auto mobilePackage = mobileRoot / fs::path{u8"휴대용 프로젝트.cstudio"};
    const auto desktopPackage = desktopRoot / mobilePackage.filename();

    ProjectPackageStore packages;
    std::optional<ProjectId> projectId;
    std::optional<StudioSnapshot> mobileStudio;
    std::optional<Timeline> initialTimeline;
    {
        auto created = packages.create(mobilePackage, utf8(u8"휴대용 프로젝트"));
        ASSERT_TRUE(created.hasValue()) << created.error().message();
        projectId = created.value().package.manifest.projectId;

        auto studioOpened =
            SqliteStudioStore::open(created.value().databasePath, *projectId);
        ASSERT_TRUE(studioOpened.hasValue()) << studioOpened.error().message();
        auto studio = std::move(studioOpened).value();
        ASSERT_TRUE(studio.seedDefaultsIfEmpty({mobileScene()}).hasValue());
        auto storedStudio = studio.load();
        ASSERT_TRUE(storedStudio.hasValue()) << storedStudio.error().message();
        mobileStudio = std::move(storedStudio).value();

        const auto video = mobileVideo();
        auto timelineOpened =
            SqliteTimelineStore::open(created.value().databasePath, *projectId);
        ASSERT_TRUE(timelineOpened.hasValue()) << timelineOpened.error().message();
        auto timelines = std::move(timelineOpened).value();
        ASSERT_TRUE(timelines.putAsset(video).hasValue());
        ASSERT_TRUE(timelines.createTimeline(mobileTimeline(video)).hasValue());
        auto storedTimeline = timelines.loadPrimaryTimeline();
        ASSERT_TRUE(storedTimeline.hasValue()) << storedTimeline.error().message();
        initialTimeline = std::move(storedTimeline).value().timeline;
    }

    std::error_code moveError;
    fs::rename(mobilePackage, desktopPackage, moveError);
    ASSERT_FALSE(moveError) << moveError.message();

    std::optional<StudioSnapshot> editedStudio;
    std::optional<Timeline> editedTimeline;
    {
        auto opened = packages.open(desktopPackage);
        ASSERT_TRUE(opened.hasValue()) << opened.error().message();
        EXPECT_EQ(opened.value().package.manifest.projectId, *projectId);
        EXPECT_EQ(opened.value().package.manifest.name,
                  utf8(u8"휴대용 프로젝트"));
        const auto manifestText = readText(desktopPackage / "manifest.json");
        EXPECT_EQ(manifestText.find("\"platform\""), std::string::npos);
        EXPECT_EQ(manifestText.find("Android-"), std::string::npos);
        EXPECT_EQ(manifestText.find("Desktop-"), std::string::npos);

        auto studioOpened =
            SqliteStudioStore::open(opened.value().databasePath, *projectId);
        ASSERT_TRUE(studioOpened.hasValue()) << studioOpened.error().message();
        auto studio = std::move(studioOpened).value();
        auto loadedStudio = studio.load();
        ASSERT_TRUE(loadedStudio.hasValue()) << loadedStudio.error().message();
        EXPECT_EQ(loadedStudio.value(), *mobileStudio);
        auto renamed = loadedStudio.value().scenes.front().withName(
            utf8(u8"데스크톱에서 편집한 장면"));
        ASSERT_TRUE(renamed.hasValue()) << renamed.error().message();
        auto scenes = loadedStudio.value().scenes;
        scenes.front() = std::move(renamed).value();
        editedStudio = StudioSnapshot{
            .scenes = std::move(scenes),
            .activeSceneId = loadedStudio.value().activeSceneId};
        ASSERT_TRUE(studio.commitSceneMutation(*editedStudio).hasValue());

        auto timelineOpened =
            SqliteTimelineStore::open(opened.value().databasePath, *projectId);
        ASSERT_TRUE(timelineOpened.hasValue()) << timelineOpened.error().message();
        auto timelines = std::move(timelineOpened).value();
        auto loadedTimeline = timelines.loadPrimaryTimeline();
        ASSERT_TRUE(loadedTimeline.hasValue()) << loadedTimeline.error().message();
        EXPECT_EQ(loadedTimeline.value().timeline, *initialTimeline);
        editedTimeline = loadedTimeline.value().timeline;
        SplitClipCommand split{
            CommandId::create("desktop-split").value(),
            TrackId::create("video").value(),
            ClipId::create("video-clip").value(),
            ClipId::create("video-clip-right").value(), at(3 * kSecond)};
        ASSERT_TRUE(split.execute(*editedTimeline).hasValue());
        ASSERT_TRUE(timelines.commitEdit(
                                  TimelineCommit{
                                      .snapshot = *editedTimeline,
                                      .expectedRevision = 0,
                                      .event = EditEventRecord{
                                          .eventId = "desktop-edit-event",
                                          .kind = EditEventKind::Apply,
                                          .command = split.record(),
                                          .createdAt = creator::core::Utc::parseRfc3339(
                                                           "2026-07-20T00:00:00Z")
                                                           .value()},
                                      .historyCount = 1,
                                      .historyCursor = 1,
                                      .cleanCursor = std::size_t{0}})
                        .hasValue());
    }

    auto reopened = packages.open(desktopPackage);
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
    auto studioReopened =
        SqliteStudioStore::open(reopened.value().databasePath, *projectId);
    ASSERT_TRUE(studioReopened.hasValue()) << studioReopened.error().message();
    auto finalStudioStore = std::move(studioReopened).value();
    auto finalStudio = finalStudioStore.load();
    ASSERT_TRUE(finalStudio.hasValue()) << finalStudio.error().message();
    EXPECT_EQ(finalStudio.value(), *editedStudio);

    auto timelineReopened =
        SqliteTimelineStore::open(reopened.value().databasePath, *projectId);
    ASSERT_TRUE(timelineReopened.hasValue()) << timelineReopened.error().message();
    auto finalTimelineStore = std::move(timelineReopened).value();
    auto finalTimeline = finalTimelineStore.loadPrimaryTimeline();
    ASSERT_TRUE(finalTimeline.hasValue()) << finalTimeline.error().message();
    EXPECT_EQ(finalTimeline.value().timeline, *editedTimeline);
    EXPECT_EQ(finalTimeline.value().revision, 1);
    EXPECT_EQ(finalTimeline.value().historyCount, 1U);
    EXPECT_EQ(finalTimeline.value().historyCursor, 1U);
    auto finalAssets = finalTimelineStore.assets();
    ASSERT_TRUE(finalAssets.hasValue()) << finalAssets.error().message();
    ASSERT_EQ(finalAssets.value().size(), 1U);
    EXPECT_EQ(finalAssets.value().front(), mobileVideo());
}

}  // namespace
