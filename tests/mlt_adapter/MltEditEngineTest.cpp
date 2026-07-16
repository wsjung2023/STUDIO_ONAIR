#include "mlt_adapter/MltEditEngine.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace creator;

void writeMonoWave(const fs::path& path, std::int16_t sampleValue) {
    constexpr std::uint32_t kSampleRate = 48'000;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBitsPerSample = 16;
    constexpr std::uint32_t kSampleCount = kSampleRate;
    constexpr std::uint32_t kDataBytes = kSampleCount * sizeof(std::int16_t);
    std::ofstream output(path, std::ios::binary);
    const auto write16 = [&](std::uint16_t value) {
        output.put(static_cast<char>(value & 0xffU));
        output.put(static_cast<char>((value >> 8U) & 0xffU));
    };
    const auto write32 = [&](std::uint32_t value) {
        write16(static_cast<std::uint16_t>(value & 0xffffU));
        write16(static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
    };
    output.write("RIFF", 4);
    write32(36U + kDataBytes);
    output.write("WAVEfmt ", 8);
    write32(16);
    write16(1);
    write16(kChannels);
    write32(kSampleRate);
    write32(kSampleRate * kChannels * (kBitsPerSample / 8U));
    write16(kChannels * (kBitsPerSample / 8U));
    write16(kBitsPerSample);
    output.write("data", 4);
    write32(kDataBytes);
    for (std::uint32_t sample = 0; sample < kSampleCount; ++sample) {
        write16(static_cast<std::uint16_t>(sampleValue));
    }
}

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
        image.close();
        std::ofstream redWide(root_ / "media/red-wide.ppm", std::ios::binary);
        redWide << "P6\n4 2\n255\n";
        for (int pixel = 0; pixel < 8; ++pixel) {
            redWide.put(static_cast<char>(255));
            redWide.put(static_cast<char>(0));
            redWide.put(static_cast<char>(0));
        }
        redWide.close();
        std::ofstream overlay(root_ / "media/blue-wide.ppm", std::ios::binary);
        overlay << "P6\n4 2\n255\n";
        for (int pixel = 0; pixel < 8; ++pixel) {
            overlay.put(static_cast<char>(0));
            overlay.put(static_cast<char>(0));
            overlay.put(static_cast<char>(255));
        }
        std::ofstream malformed(root_ / "media/malformed.bin",
                                std::ios::binary);
        malformed << "not a media container";
        writeMonoWave(root_ / "media/microphone.wav", 1000);
        writeMonoWave(root_ / "media/system.wav", -1000);
    }
    ~MltFixture() { fs::remove_all(root_); }
    const fs::path& root() const { return root_; }

private:
    fs::path root_;
};

edit_engine::TimelineSnapshot imageSnapshot(const fs::path& root,
                                             std::string assetId,
                                             std::string relativePath,
                                             std::int64_t revision) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto asset = domain::MediaAsset::create(
                     domain::AssetId::create(std::move(assetId)).value(),
                     domain::MediaKind::Image, std::move(relativePath),
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
    const auto range = domain::TimeRange::create(
                           core::TimestampNs{},
                           core::DurationNs{1'000'000'000})
                           .value();
    EXPECT_TRUE(timeline.insertClip(
        trackId,
        domain::Clip::createAsset(domain::ClipId::create("image-clip").value(),
                                  asset, range, range, true, std::nullopt,
                                  std::nullopt)
            .value())
                    .hasValue());
    return edit_engine::TimelineSnapshot{
        std::move(timeline), domain::TimelineRevision::create(revision).value(),
        {std::move(asset)}, root};
}

edit_engine::TimelineSnapshot snapshot(const fs::path& root,
                                       std::int64_t revision = 1) {
    return imageSnapshot(root, "red", "media/red.ppm", revision);
}

edit_engine::TimelineSnapshot layeredSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto makeAsset = [&](std::string id, std::string path) {
        return domain::MediaAsset::create(
                   domain::AssetId::create(std::move(id)).value(),
                   domain::MediaKind::Image, std::move(path),
                   core::DurationNs{1'000'000'000},
                   domain::VideoAssetMetadata{.width = 4, .height = 2,
                                              .frameRate = rate},
                   std::nullopt, 100, "fixture",
                   domain::AssetAvailability::Available)
            .value();
    };
    auto red = makeAsset("red-wide", "media/red-wide.ppm");
    auto overlay = makeAsset("blue-wide", "media/blue-wide.ppm");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("layered").value(),
                        "Layered", rate)
                        .value();
    const auto bottom = domain::TrackId::create("bottom").value();
    const auto top = domain::TrackId::create("top").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      bottom, domain::TrackKind::Video,
                                      "Bottom", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      top, domain::TrackKind::Video,
                                      "Top", true, false)
                                      .value())
                    .hasValue());
    const auto fullRange = domain::TimeRange::create(
                               core::TimestampNs{},
                               core::DurationNs{1'000'000'000})
                               .value();
    const auto upperTimelineRange = domain::TimeRange::create(
                                        core::TimestampNs{
                                            core::DurationNs{500'000'000}},
                                        core::DurationNs{500'000'000})
                                        .value();
    const auto upperSourceRange = domain::TimeRange::create(
                                      core::TimestampNs{},
                                      core::DurationNs{500'000'000})
                                      .value();
    EXPECT_TRUE(timeline.insertClip(
        bottom,
        domain::Clip::createAsset(domain::ClipId::create("bottom-clip").value(),
                                  red, fullRange, fullRange, true, std::nullopt,
                                  std::nullopt)
            .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
        top,
        domain::Clip::createAsset(domain::ClipId::create("top-clip").value(),
                                  overlay, upperSourceRange, upperTimelineRange,
                                  true, std::nullopt,
                                  std::nullopt)
            .value())
                    .hasValue());
    return edit_engine::TimelineSnapshot{
        std::move(timeline), domain::TimelineRevision::create(2).value(),
        {std::move(red), std::move(overlay)}, root};
}

edit_engine::TimelineSnapshot audioMixSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto video = domain::MediaAsset::create(
                     domain::AssetId::create("red-video").value(),
                     domain::MediaKind::Image, "media/red.ppm",
                     core::DurationNs{1'000'000'000},
                     domain::VideoAssetMetadata{.width = 2, .height = 2,
                                                .frameRate = rate},
                     std::nullopt, 23, "video-fixture",
                     domain::AssetAvailability::Available)
                     .value();
    auto makeAudio = [](std::string id, std::string path) {
        return domain::MediaAsset::create(
                   domain::AssetId::create(std::move(id)).value(),
                   domain::MediaKind::Audio, std::move(path),
                   core::DurationNs{1'000'000'000}, std::nullopt,
                   domain::AudioAssetMetadata{48'000, 1}, 96'044,
                   "audio-fixture", domain::AssetAvailability::Available)
            .value();
    };
    auto microphone = makeAudio("microphone", "media/microphone.wav");
    auto system = makeAudio("system", "media/system.wav");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("audio-mix").value(),
                        "Audio mix", rate)
                        .value();
    const auto videoTrack = domain::TrackId::create("video").value();
    const auto microphoneTrack = domain::TrackId::create("microphone").value();
    const auto systemTrack = domain::TrackId::create("system").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      videoTrack, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      microphoneTrack, domain::TrackKind::Audio,
                                      "Microphone", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      systemTrack, domain::TrackKind::Audio,
                                      "System", true, false)
                                      .value())
                    .hasValue());
    const auto range = domain::TimeRange::create(core::TimestampNs{},
                                                  core::DurationNs{1'000'000'000})
                           .value();
    const auto insert = [&](const domain::TrackId& track,
                            std::string clipId,
                            const domain::MediaAsset& asset) {
        return timeline.insertClip(
            track,
            domain::Clip::createAsset(
                domain::ClipId::create(std::move(clipId)).value(), asset,
                range, range, true, std::nullopt, std::nullopt)
                .value());
    };
    EXPECT_TRUE(insert(videoTrack, "video-clip", video).hasValue());
    EXPECT_TRUE(insert(microphoneTrack, "microphone-clip", microphone).hasValue());
    EXPECT_TRUE(insert(systemTrack, "system-clip", system).hasValue());
    return edit_engine::TimelineSnapshot{
        std::move(timeline), domain::TimelineRevision::create(5).value(),
        {std::move(video), std::move(microphone), std::move(system)}, root};
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
    ASSERT_TRUE(engine.seek(core::TimestampNs{}).hasValue());
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
    auto laterFrame = engine.requestFrame(
        core::TimestampNs{core::DurationNs{500'000'000}});
    ASSERT_TRUE(laterFrame.hasValue()) << laterFrame.error().message();
    const auto* laterBgra = static_cast<const std::uint8_t*>(
        laterFrame.value().frame().platformHandle.get());
    ASSERT_NE(laterBgra, nullptr);
    EXPECT_LT(laterBgra[0], 8U);
    EXPECT_LT(laterBgra[1], 8U);
    EXPECT_GT(laterBgra[2], 247U);
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

TEST(MltEditEngineTest, CompositesUpperVideoTrackOverLowerTrack) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(layeredSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto lowerFrame = engine.requestFrame(core::TimestampNs{});
    auto upperFrame = engine.requestFrame(
        core::TimestampNs{core::DurationNs{600'000'000}});

    ASSERT_TRUE(lowerFrame.hasValue()) << lowerFrame.error().message();
    ASSERT_TRUE(upperFrame.hasValue()) << upperFrame.error().message();
    const auto* lowerBgra = static_cast<const std::uint8_t*>(
        lowerFrame.value().frame().platformHandle.get());
    const auto* upperBgra = static_cast<const std::uint8_t*>(
        upperFrame.value().frame().platformHandle.get());
    ASSERT_NE(lowerBgra, nullptr);
    ASSERT_NE(upperBgra, nullptr);
    // Before the upper clip begins the lower track is red.
    EXPECT_LT(lowerBgra[0], 16U);
    EXPECT_LT(lowerBgra[1], 16U);
    EXPECT_GT(lowerBgra[2], 239U);
    // During the overlap the upper track is blue.
    EXPECT_GT(upperBgra[0], 239U);
    EXPECT_LT(upperBgra[1], 16U);
    EXPECT_LT(upperBgra[2], 16U);
}

TEST(MltEditEngineTest, SuccessfulUpdatePublishesNewRevision) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};
    ASSERT_TRUE(engine.load(snapshot(fixture.root(), 1)).hasValue());
    auto change = edit_engine::TimelineChangeSet::create(
                      domain::TimelineRevision::create(1).value(),
                      snapshot(fixture.root(), 2),
                      {domain::TrackId::create("video").value()}, true)
                      .value();

    auto updated = engine.update(change);

    ASSERT_TRUE(updated.hasValue()) << updated.error().message();
    auto frame = engine.requestFrame(core::TimestampNs{});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().revision().value(), 2);
}

TEST(MltEditEngineTest, FailedUpdatePreservesLastGoodGraph) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};
    ASSERT_TRUE(engine.load(snapshot(fixture.root(), 3)).hasValue());
    auto malformed = imageSnapshot(fixture.root(), "malformed",
                                   "media/malformed.bin", 4);
    auto change = edit_engine::TimelineChangeSet::create(
                      domain::TimelineRevision::create(3).value(),
                      std::move(malformed),
                      {domain::TrackId::create("video").value()}, true)
                      .value();

    auto updated = engine.update(change);

    EXPECT_FALSE(updated.hasValue());
    auto frame = engine.requestFrame(core::TimestampNs{});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().revision().value(), 3);
    const auto* bgra = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(bgra, nullptr);
    EXPECT_GT(bgra[2], 247U);
}

TEST(MltEditEngineTest, LoadsTwoRealAudioTracksWithCoreMixTransitions) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};

    auto loaded = engine.load(audioMixSnapshot(fixture.root()));

    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    const auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue()) << diagnostics.error().message();
    EXPECT_EQ(diagnostics.value().nativeTrackCount, 4U);
    EXPECT_EQ(diagnostics.value().videoCompositeTransitions, 1U);
    EXPECT_EQ(diagnostics.value().audioMixTransitions, 2U);
    auto frame = engine.requestFrame(core::TimestampNs{});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().revision().value(), 5);
}

TEST(MltEditEngineTest, RepeatedEngineDestructionDoesNotCorruptNextGraph) {
    MltFixture fixture;
    for (int iteration = 0; iteration < 3; ++iteration) {
        mlt_adapter::MltEditEngine engine{{
            .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
            .previewWidth = 2,
            .previewHeight = 2}};
        auto loaded = engine.load(snapshot(fixture.root(), iteration + 10));
        ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
        auto frame = engine.requestFrame(core::TimestampNs{});
        ASSERT_TRUE(frame.hasValue()) << frame.error().message();
        EXPECT_EQ(frame.value().revision().value(), iteration + 10);
        const auto* bgra = static_cast<const std::uint8_t*>(
            frame.value().frame().platformHandle.get());
        ASSERT_NE(bgra, nullptr);
        EXPECT_GT(bgra[2], 239U);
    }
}

}  // namespace
