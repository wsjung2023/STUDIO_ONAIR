#include "mlt_adapter/MltGraphPlan.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace creator;

core::TimestampNs at(std::int64_t nanoseconds) {
    return core::TimestampNs{core::DurationNs{nanoseconds}};
}

class TemporaryPackage final {
public:
    TemporaryPackage() {
        auto name = std::u8string{u8"creator-studio-mlt-그래프-"};
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (const auto character : suffix) {
            name.push_back(static_cast<char8_t>(character));
        }
        root_ = fs::temp_directory_path() / fs::path{name};
        fs::remove_all(root_);
        fs::create_directories(root_ / "media");
    }
    ~TemporaryPackage() { fs::remove_all(root_); }
    const fs::path& root() const { return root_; }

    void addFile(const fs::path& relative) const {
        fs::create_directories((root_ / relative).parent_path());
        std::ofstream stream(root_ / relative, std::ios::binary);
        stream << "fixture";
    }

private:
    fs::path root_;
};

domain::MediaAsset videoAsset(std::string id, std::string path,
                              domain::AssetAvailability availability,
                              bool withAudio = false) {
    return domain::MediaAsset::create(
               domain::AssetId::create(std::move(id)).value(),
               domain::MediaKind::Video, path, core::DurationNs{2'000'000'000},
               domain::VideoAssetMetadata{
                   .width = 1920,
                   .height = 1080,
                   .frameRate = core::FrameRate::create(30, 1).value()},
               withAudio
                   ? std::optional<domain::AudioAssetMetadata>{
                         domain::AudioAssetMetadata{.sampleRate = 48'000,
                                                    .channels = 2}}
                   : std::nullopt,
               7, "hash", availability)
        .value();
}

domain::MediaAsset audioAsset(std::string id, std::string path) {
    return domain::MediaAsset::create(
               domain::AssetId::create(std::move(id)).value(),
               domain::MediaKind::Audio, path,
               core::DurationNs{2'000'000'000}, std::nullopt,
               domain::AudioAssetMetadata{.sampleRate = 48'000, .channels = 2},
               7, "audio-hash", domain::AssetAvailability::Available)
        .value();
}

domain::VisualTransform transform(std::int32_t zOrder) {
    return domain::VisualTransform::create(
               0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0,
               0.0, 1.0, zOrder)
        .value();
}

domain::Timeline timelineWithClip(const domain::MediaAsset& asset,
                                  std::int64_t sourceStart,
                                  std::int64_t timelineStart,
                                  std::int64_t duration,
                                  std::optional<std::int64_t> timelineDuration =
                                      std::nullopt) {
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main",
                        core::FrameRate::create(30, 1).value())
                        .value();
    const auto trackId = domain::TrackId::create("video-1").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      trackId, domain::TrackKind::Video,
                                      "Screen", true, false)
                                      .value())
                    .hasValue());
    auto clip = domain::Clip::createAsset(
        domain::ClipId::create("clip-1").value(), asset,
        domain::TimeRange::create(at(sourceStart), core::DurationNs{duration})
            .value(),
        domain::TimeRange::create(
            at(timelineStart),
            core::DurationNs{timelineDuration.value_or(duration)})
            .value(),
        true, std::nullopt, std::nullopt);
    EXPECT_TRUE(clip.hasValue());
    EXPECT_TRUE(timeline.insertClip(trackId, std::move(clip).value()).hasValue());
    return timeline;
}

TEST(MltGraphPlanTest, ResolvesUnicodeMediaAndPreservesGapAndTrimFrames) {
    TemporaryPackage package;
    const fs::path relative = fs::path{"media"} / fs::path{u8"화면.bmp"};
    package.addFile(relative);
    const auto relativeUtf8 = relative.generic_u8string();
    auto asset = videoAsset("screen",
                            std::string{relativeUtf8.begin(), relativeUtf8.end()},
                            domain::AssetAvailability::Available);
    edit_engine::TimelineSnapshot snapshot{
        timelineWithClip(asset, 500'000'000, 1'000'000'000, 1'000'000'000),
        domain::TimelineRevision::create(4).value(),
        {asset},
        package.root()};

    auto plan = mlt_adapter::compileMltGraphPlan(snapshot);

    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    ASSERT_EQ(plan.value().tracks.size(), 1U);
    ASSERT_EQ(plan.value().tracks.front().clips.size(), 1U);
    const auto& clip = plan.value().tracks.front().clips.front();
    EXPECT_EQ(clip.sourceIn, 15);
    EXPECT_EQ(clip.sourceOut, 44);
    EXPECT_EQ(clip.timelineIn, 30);
    EXPECT_EQ(clip.timelineOut, 59);
    EXPECT_EQ(clip.mediaPath, fs::weakly_canonical(package.root() / relative));
    EXPECT_EQ(plan.value().durationFrames, 60);
    EXPECT_EQ(plan.value().revision.value(), 4);
}

TEST(MltGraphPlanTest,
     TrimsSubframePhaseMismatchToSharedCompleteFrameBoundary) {
    TemporaryPackage package;
    package.addFile("media/physical.mkv");
    const auto asset = videoAsset("physical", "media/physical.mkv",
                                  domain::AssetAvailability::Available);
    edit_engine::TimelineSnapshot snapshot{
        timelineWithClip(asset, 0, 20'000'000, 50'000'000),
        domain::TimelineRevision::create(1).value(), {asset}, package.root()};

    auto plan = mlt_adapter::compileMltGraphPlan(snapshot);

    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    ASSERT_EQ(plan.value().tracks.front().clips.size(), 1U);
    const auto& clip = plan.value().tracks.front().clips.front();
    EXPECT_EQ(clip.sourceIn, 0);
    EXPECT_EQ(clip.sourceOut, 0);
    EXPECT_EQ(clip.timelineIn, 0);
    EXPECT_EQ(clip.timelineOut, 0);
    ASSERT_EQ(plan.value().diagnostics.size(), 1U);
    EXPECT_NE(plan.value().diagnostics.front().find("complete matching frame"),
              std::string::npos);
}

TEST(MltGraphPlanTest, KeepsPositiveSubFrameTimelineAsOneOutputFrame) {
    TemporaryPackage package;
    package.addFile("media/physical.mkv");
    const auto asset = videoAsset("physical", "media/physical.mkv",
                                  domain::AssetAvailability::Available);
    edit_engine::TimelineSnapshot snapshot{
        timelineWithClip(asset, 0, 0, 1),
        domain::TimelineRevision::create(1).value(), {asset}, package.root()};

    auto plan = mlt_adapter::compileMltGraphPlan(snapshot);

    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    ASSERT_EQ(plan.value().tracks.front().clips.size(), 1U);
    EXPECT_EQ(plan.value().tracks.front().clips.front().timelineIn, 0);
    EXPECT_EQ(plan.value().tracks.front().clips.front().timelineOut, 0);
}

TEST(MltGraphPlanTest, KeepsOfflineClipExplicitWithoutResolvingAFile) {
    TemporaryPackage package;
    auto asset = videoAsset("offline", "media/missing.mkv",
                            domain::AssetAvailability::Offline);
    edit_engine::TimelineSnapshot snapshot{
        timelineWithClip(asset, 0, 0, 1'000'000'000),
        domain::TimelineRevision::create(1).value(),
        {asset},
        package.root()};

    auto plan = mlt_adapter::compileMltGraphPlan(snapshot);

    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    ASSERT_EQ(plan.value().tracks.front().clips.size(), 1U);
    EXPECT_FALSE(plan.value().tracks.front().clips.front().available);
    EXPECT_TRUE(plan.value().tracks.front().clips.front().mediaPath.empty());
    ASSERT_EQ(plan.value().visualBranches.size(), 1U);
    EXPECT_FALSE(plan.value().visualBranches.front().available);
    EXPECT_TRUE(plan.value().visualBranches.front().enabled);
    EXPECT_EQ(plan.value().visualBranches.front().clipId.value(), "clip-1");
    EXPECT_EQ(plan.value().durationFrames, 30);
}

TEST(MltGraphPlanTest, KeepsDisabledVisualBranchesExplicitAndOrdered) {
    TemporaryPackage package;
    package.addFile("media/video.bin");
    const auto asset = videoAsset("video", "media/video.bin",
                                  domain::AssetAvailability::Available);
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main",
                        core::FrameRate::create(30, 1).value())
                        .value();
    const auto disabledTrack = domain::TrackId::create("video-off").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(disabledTrack,
                                                  domain::TrackKind::Video,
                                                  "Off", false, false)
                                .value())
                    .hasValue());
    const auto full = domain::TimeRange::create(
                          at(0), core::DurationNs{2'000'000'000})
                          .value();
    ASSERT_TRUE(timeline.insertClip(
                            disabledTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("track-disabled").value(),
                                asset, full, full, true, transform(-1),
                                std::nullopt)
                                .value())
                    .hasValue());
    const auto enabledTrack = domain::TrackId::create("video-on").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(enabledTrack,
                                                  domain::TrackKind::Video,
                                                  "On", true, false)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            enabledTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("clip-disabled").value(),
                                asset, full, full, false, transform(1),
                                std::nullopt)
                                .value())
                    .hasValue());
    edit_engine::TimelineSnapshot snapshot{
        std::move(timeline), domain::TimelineRevision::create(2).value(),
        {asset}, package.root()};

    auto plan = mlt_adapter::compileMltGraphPlan(snapshot);

    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    ASSERT_EQ(plan.value().visualBranches.size(), 2U);
    EXPECT_EQ(plan.value().visualBranches[0].clipId.value(),
              "track-disabled");
    EXPECT_EQ(plan.value().visualBranches[1].clipId.value(),
              "clip-disabled");
    EXPECT_TRUE(plan.value().visualBranches[0].available);
    EXPECT_TRUE(plan.value().visualBranches[1].available);
    EXPECT_FALSE(plan.value().visualBranches[0].enabled);
    EXPECT_FALSE(plan.value().visualBranches[1].enabled);
    EXPECT_EQ(plan.value().durationFrames, 60);
}

TEST(MltGraphPlanTest, RejectsDuplicateAssetsMissingFilesAndMissingRoot) {
    TemporaryPackage package;
    auto missing = videoAsset("asset", "media/missing.mkv",
                              domain::AssetAvailability::Available);
    auto duplicate = missing;
    edit_engine::TimelineSnapshot duplicateSnapshot{
        timelineWithClip(missing, 0, 0, 1'000'000'000),
        domain::TimelineRevision::create(1).value(),
        {missing, duplicate},
        package.root()};
    edit_engine::TimelineSnapshot missingSnapshot{
        duplicateSnapshot.timeline,
        duplicateSnapshot.revision,
        {missing},
        package.root()};
    auto missingRootSnapshot = missingSnapshot;
    missingRootSnapshot.mediaRoot.clear();

    EXPECT_FALSE(mlt_adapter::compileMltGraphPlan(duplicateSnapshot).hasValue());
    EXPECT_FALSE(mlt_adapter::compileMltGraphPlan(missingSnapshot).hasValue());
    EXPECT_FALSE(
        mlt_adapter::compileMltGraphPlan(missingRootSnapshot).hasValue());
}

TEST(MltGraphPlanTest, SeparatesAudioAndOrdersAssetTitleAndCaptionBranches) {
    TemporaryPackage package;
    package.addFile("media/video.bin");
    package.addFile("audio/audio.bin");
    package.addFile("cache/generated/title.png");
    package.addFile("cache/generated/cue-a.png");
    package.addFile("cache/generated/cue-b.png");
    const auto video = videoAsset("video", "media/video.bin",
                                  domain::AssetAvailability::Available, true);
    const auto audio = audioAsset("audio", "audio/audio.bin");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main",
                        core::FrameRate::create(30, 1).value())
                        .value();
    const auto videoTrack = domain::TrackId::create("video-1").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(videoTrack,
                                                  domain::TrackKind::Video,
                                                  "Video", true, false)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            videoTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("video-high").value(),
                                video,
                                domain::TimeRange::create(
                                    at(0), core::DurationNs{1'000'000'000})
                                    .value(),
                                domain::TimeRange::create(
                                    at(0), core::DurationNs{1'000'000'000})
                                    .value(),
                                true, transform(5), std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            videoTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("video-low").value(),
                                video,
                                domain::TimeRange::create(
                                    at(1'000'000'000),
                                    core::DurationNs{1'000'000'000})
                                    .value(),
                                domain::TimeRange::create(
                                    at(1'000'000'000),
                                    core::DurationNs{1'000'000'000})
                                    .value(),
                                true, transform(-2), std::nullopt)
                                .value())
                    .hasValue());

    const auto audioTrack = domain::TrackId::create("audio-1").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(audioTrack,
                                                  domain::TrackKind::Audio,
                                                  "Audio", true, false)
                                .value())
                    .hasValue());
    const auto full = domain::TimeRange::create(
                          at(0), core::DurationNs{2'000'000'000})
                          .value();
    ASSERT_TRUE(timeline.insertClip(
                            audioTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("audio-clip").value(),
                                audio, full, full, true, std::nullopt,
                                domain::AudioEnvelope::create(
                                    -6.0, core::DurationNs{100'000'000},
                                    core::DurationNs{200'000'000},
                                    full.duration())
                                    .value())
                                .value())
                    .hasValue());

    const auto titleTrack = domain::TrackId::create("title-1").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(titleTrack,
                                                  domain::TrackKind::Title,
                                                  "Titles", true, false)
                                .value())
                    .hasValue());
    const auto titleRange = domain::TimeRange::create(
                                at(100'000'000),
                                core::DurationNs{400'000'000})
                                .value();
    const auto titlePayload = domain::TitlePayload::create(
                                  "Title", "Arial", 0.5, 0.3,
                                  domain::RgbaColor::parse("#ffffffff").value(),
                                  domain::RgbaColor::parse("#00000000").value(),
                                  domain::TextAlignment::Center)
                                  .value();
    ASSERT_TRUE(timeline.insertClip(
                            titleTrack,
                            domain::Clip::createTitle(
                                domain::ClipId::create("title-clip").value(),
                                titleRange, true, titlePayload,
                                domain::VisualTransform::create(
                                    0.2, 0.2, 0.5, 0.5, 1.0, 1.0, 15.0,
                                    0.0, 0.0, 0.0, 0.0, 0.8, 0)
                                    .value())
                                .value())
                    .hasValue());

    const auto captionTrack = domain::TrackId::create("caption-1").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(captionTrack,
                                                  domain::TrackKind::Caption,
                                                  "Captions", true, false)
                                .value())
                    .hasValue());
    const auto captionRange = domain::TimeRange::create(
                                  at(0), core::DurationNs{1'000'000'000})
                                  .value();
    const auto cueA = domain::CaptionCue::create(
                          domain::CueId::create("cue-a").value(),
                          core::DurationNs{0}, core::DurationNs{200'000'000},
                          "A")
                          .value();
    const auto cueB = domain::CaptionCue::create(
                          domain::CueId::create("cue-b").value(),
                          core::DurationNs{500'000'000},
                          core::DurationNs{200'000'000}, "B")
                          .value();
    ASSERT_TRUE(timeline.insertClip(
                            captionTrack,
                            domain::Clip::createCaption(
                                domain::ClipId::create("caption-clip").value(),
                                captionRange, true, {cueA, cueB}, std::nullopt)
                                .value())
                    .hasValue());

    const auto descriptor = [&](std::string file, domain::ClipId clip,
                                std::optional<domain::CueId> cue,
                                domain::TimeRange range) {
        return edit_engine::GeneratedOverlayDescriptor::create(
                   std::move(clip), std::move(cue),
                   fs::path{"cache/generated"} / std::move(file), range,
                   "Arial")
            .value();
    };
    edit_engine::TimelineSnapshot snapshot{
        std::move(timeline), domain::TimelineRevision::create(9).value(),
        {video, audio}, package.root(), 1280, 720,
        {descriptor("title.png", domain::ClipId::create("title-clip").value(),
                    std::nullopt, titleRange),
         descriptor("cue-a.png",
                    domain::ClipId::create("caption-clip").value(), cueA.id(),
                    domain::TimeRange::create(at(0),
                                              core::DurationNs{200'000'000})
                        .value()),
         descriptor("cue-b.png",
                    domain::ClipId::create("caption-clip").value(), cueB.id(),
                    domain::TimeRange::create(at(500'000'000),
                                              core::DurationNs{200'000'000})
                        .value())}};

    auto plan = mlt_adapter::compileMltGraphPlan(snapshot);

    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    EXPECT_EQ(plan.value().canvasWidth, 1280);
    EXPECT_EQ(plan.value().canvasHeight, 720);
    ASSERT_EQ(plan.value().audioTracks.size(), 2U);
    ASSERT_EQ(plan.value().audioTracks[0].clips.size(), 2U);
    EXPECT_EQ(plan.value().audioTracks[0].kind, domain::TrackKind::Video);
    ASSERT_EQ(plan.value().audioTracks[1].clips.size(), 1U);
    ASSERT_EQ(plan.value().visualBranches.size(), 5U);
    EXPECT_EQ(plan.value().visualBranches[0].clipId.value(), "video-low");
    EXPECT_EQ(plan.value().visualBranches[1].clipId.value(), "title-clip");
    EXPECT_EQ(plan.value().visualBranches[2].cueId->value(), "cue-a");
    EXPECT_EQ(plan.value().visualBranches[3].cueId->value(), "cue-b");
    EXPECT_EQ(plan.value().visualBranches[4].clipId.value(), "video-high");
    EXPECT_EQ(plan.value().visualBranches[0].order.zOrder, -2);
    EXPECT_EQ(plan.value().visualBranches[4].order.zOrder, 5);
    EXPECT_EQ(plan.value().visualBranches[1].sourceKind,
              mlt_adapter::MltVisualSourceKind::Generated);
    EXPECT_EQ(plan.value().visualBranches[1].transform, transform(0));
    EXPECT_EQ(plan.value().durationFrames, 60);
    EXPECT_TRUE(plan.value().diagnostics.empty());
}

TEST(MltGraphPlanTest, KeepsMissingGeneratedOverlayExplicitAndRejectsBadOwner) {
    TemporaryPackage package;
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main",
                        core::FrameRate::create(30, 1).value())
                        .value();
    const auto trackId = domain::TrackId::create("title-1").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(trackId,
                                                  domain::TrackKind::Title,
                                                  "Titles", true, false)
                                .value())
                    .hasValue());
    const auto range = domain::TimeRange::create(
                           at(0), core::DurationNs{1'000'000'000})
                           .value();
    const auto payload = domain::TitlePayload::create(
                             "Missing", "Arial", 0.5, 0.5,
                             domain::RgbaColor::parse("#ffffffff").value(),
                             domain::RgbaColor::parse("#00000000").value(),
                             domain::TextAlignment::Center)
                             .value();
    ASSERT_TRUE(timeline.insertClip(
                            trackId,
                            domain::Clip::createTitle(
                                domain::ClipId::create("title-clip").value(),
                                range, true, payload, std::nullopt)
                                .value())
                    .hasValue());
    edit_engine::TimelineSnapshot snapshot{
        timeline, domain::TimelineRevision::create(1).value(), {},
        package.root(), 1920, 1080};

    auto missing = mlt_adapter::compileMltGraphPlan(snapshot);
    ASSERT_TRUE(missing.hasValue()) << missing.error().message();
    ASSERT_EQ(missing.value().visualBranches.size(), 1U);
    EXPECT_FALSE(missing.value().visualBranches[0].available);
    ASSERT_EQ(missing.value().diagnostics.size(), 1U);
    EXPECT_NE(missing.value().diagnostics[0].find("title-clip"),
              std::string::npos);

    snapshot.generatedOverlays.push_back(
        edit_engine::GeneratedOverlayDescriptor::create(
            domain::ClipId::create("unknown-clip").value(), std::nullopt,
            "cache/generated/unknown.png", range, "Arial")
            .value());
    EXPECT_FALSE(mlt_adapter::compileMltGraphPlan(snapshot).hasValue());
}

}  // namespace
