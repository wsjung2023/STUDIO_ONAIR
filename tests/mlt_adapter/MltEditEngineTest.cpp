#include "mlt_adapter/MltEditEngine.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace creator;

class MltFixture final {
public:
    MltFixture() {
        root_ = fs::temp_directory_path() /
                ("creator-studio-mlt-engine-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        fs::create_directories(root_ / "media");
        std::ofstream image(root_ / "media/red.ppm", std::ios::binary);
        image << "P6\n2 2\n255\n";
        for (int pixel = 0; pixel < 4; ++pixel) {
            image.put(static_cast<char>(255));
            image.put(static_cast<char>(0));
            image.put(static_cast<char>(0));
        }
    }
    ~MltFixture() { fs::remove_all(root_); }
    const fs::path& root() const { return root_; }

private:
    fs::path root_;
};

edit_engine::TimelineSnapshot snapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto asset = domain::MediaAsset::create(
                     domain::AssetId::create("red").value(),
                     domain::MediaKind::Image, "media/red.ppm",
                     core::DurationNs{1'000'000'000},
                     domain::VideoAssetMetadata{.width = 2, .height = 2,
                                                .frameRate = rate},
                     std::nullopt, 23, "fixture",
                     domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main", rate)
                        .value();
    const auto trackId = domain::TrackId::create("video").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      trackId, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    const auto range = domain::TimeRange::create(core::TimestampNs{},
                                                  core::DurationNs{1'000'000'000})
                           .value();
    EXPECT_TRUE(timeline.insertClip(
        trackId,
        domain::Clip::createAsset(domain::ClipId::create("red-clip").value(),
                                  asset, range, range, true, std::nullopt,
                                  std::nullopt)
            .value())
                    .hasValue());
    return edit_engine::TimelineSnapshot{
        std::move(timeline), domain::TimelineRevision::create(1).value(),
        {std::move(asset)}, root};
}

TEST(MltEditEngineTest, LoadsRealAvformatMediaAndReturnsOwnedBgraFrame) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};

    auto loaded = engine.load(snapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    ASSERT_TRUE(engine.play().hasValue());
    auto frame = engine.requestFrame(core::TimestampNs{});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().frame().width, 2U);
    EXPECT_EQ(frame.value().revision().value(), 1);
    ASSERT_NE(frame.value().frame().platformHandle, nullptr);
    const auto* bgra = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    EXPECT_LT(bgra[0], 8U);
    EXPECT_LT(bgra[1], 8U);
    EXPECT_GT(bgra[2], 247U);
    EXPECT_EQ(bgra[3], 255U);
    EXPECT_TRUE(engine.pause().hasValue());
}

TEST(MltEditEngineTest, RejectsFrameBeforeLoadAndOutsideTimeline) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};
    EXPECT_FALSE(engine.requestFrame(core::TimestampNs{}).hasValue());
    ASSERT_TRUE(engine.load(snapshot(fixture.root())).hasValue());
    EXPECT_FALSE(engine.requestFrame(core::TimestampNs{
                                         core::DurationNs{2'000'000'000}})
                     .hasValue());
}

}  // namespace
