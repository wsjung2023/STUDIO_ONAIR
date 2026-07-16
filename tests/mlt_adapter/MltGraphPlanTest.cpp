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
                              domain::AssetAvailability availability) {
    return domain::MediaAsset::create(
               domain::AssetId::create(std::move(id)).value(),
               domain::MediaKind::Video, path, core::DurationNs{2'000'000'000},
               domain::VideoAssetMetadata{
                   .width = 1920,
                   .height = 1080,
                   .frameRate = core::FrameRate::create(30, 1).value()},
               std::nullopt, 7, "hash", availability)
        .value();
}

domain::Timeline timelineWithClip(const domain::MediaAsset& asset,
                                  std::int64_t sourceStart,
                                  std::int64_t timelineStart,
                                  std::int64_t duration) {
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
        domain::TimeRange::create(at(timelineStart), core::DurationNs{duration})
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

}  // namespace
