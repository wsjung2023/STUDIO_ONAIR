#include "mlt_adapter/MltEditEngine.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace creator;

class PublicationBoundaryLifecycle final
    : public edit_engine::IRenderJobLifecycle {
public:
    core::Result<void> begin(const edit_engine::RenderRequest& request,
                             const fs::path&, core::DurationNs) override {
        destination = request.destination();
        began.store(true);
        return core::ok();
    }
    core::Result<void> encoderSelected(
        const domain::RenderJobId&,
        const edit_engine::RenderEncoderDiagnostics& diagnostics) override {
        selected.store(!diagnostics.selectedEncoder.empty());
        return core::ok();
    }
    core::Result<void> advance(
        const domain::RenderJobId&,
        const edit_engine::RenderProgress& progress) override {
        if (progress.state() == edit_engine::RenderJobState::Running) {
            ran.store(true);
        }
        return core::ok();
    }
    core::Result<void> preparePublication(
        const domain::RenderJobId&, const fs::path& partial,
        const edit_engine::RenderProgress&) override {
        preparedBeforeRename.store(fs::is_regular_file(partial) &&
                                   !fs::exists(destination));
        return core::ok();
    }
    core::Result<void> finish(const domain::RenderJobId&,
                              edit_engine::RenderJobState state,
                              std::string) override {
        completedAfterRename.store(
            state == edit_engine::RenderJobState::Completed &&
            fs::is_regular_file(destination));
        return core::ok();
    }

    fs::path destination;
    std::atomic_bool began{};
    std::atomic_bool selected{};
    std::atomic_bool ran{};
    std::atomic_bool preparedBeforeRename{};
    std::atomic_bool completedAfterRename{};
};

void writeMonoWave(const fs::path& path, std::int16_t sampleValue,
                   std::uint32_t sampleCount = 48'000) {
    constexpr std::uint32_t kSampleRate = 48'000;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBitsPerSample = 16;
    const std::uint32_t dataBytes = sampleCount * sizeof(std::int16_t);
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
    write32(36U + dataBytes);
    output.write("WAVEfmt ", 8);
    write32(16);
    write16(1);
    write16(kChannels);
    write32(kSampleRate);
    write32(kSampleRate * kChannels * (kBitsPerSample / 8U));
    write16(kChannels * (kBitsPerSample / 8U));
    write16(kBitsPerSample);
    output.write("data", 4);
    write32(dataBytes);
    for (std::uint32_t sample = 0; sample < sampleCount; ++sample) {
        write16(static_cast<std::uint16_t>(sampleValue));
    }
}

void writeNonFiniteFloatWave(const fs::path& path) {
    constexpr std::uint32_t sampleRate = 48'000;
    constexpr std::uint32_t dataBytes = sampleRate * sizeof(float);
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
    write32(36U + dataBytes);
    output.write("WAVEfmt ", 8);
    write32(16);
    write16(3);
    write16(1);
    write32(sampleRate);
    write32(sampleRate * sizeof(float));
    write16(static_cast<std::uint16_t>(sizeof(float)));
    write16(32);
    output.write("data", 4);
    write32(dataBytes);
    const float value = std::numeric_limits<float>::quiet_NaN();
    for (std::uint32_t sample = 0; sample < sampleRate; ++sample) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
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
        fs::create_directories(root_ / "cache/generated");
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
        overlay.close();
        const auto writeSolid = [&](const fs::path& relative,
                                    std::uint8_t red,
                                    std::uint8_t green,
                                    std::uint8_t blue) {
            std::ofstream imageFile(root_ / relative, std::ios::binary);
            imageFile << "P6\n4 2\n255\n";
            for (int pixel = 0; pixel < 8; ++pixel) {
                imageFile.put(static_cast<char>(red));
                imageFile.put(static_cast<char>(green));
                imageFile.put(static_cast<char>(blue));
            }
        };
        constexpr std::array<std::uint8_t, 77> transparentTitlePng{
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
            0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04,
            0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7f,
            0xa8, 0x7d, 0x63, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41,
            0x54, 0x78, 0x9c, 0x63, 0x64, 0x80, 0x81, 0xff, 0x0c, 0xff,
            0x41, 0x14, 0x13, 0x5c, 0x00, 0x0a, 0x00, 0x2e, 0x38, 0x02,
            0x02, 0x14, 0xc1, 0x03, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4e, 0x44, 0xae,
            0x42, 0x60, 0x82};
        std::ofstream titlePng(root_ / "cache/generated/title.png",
                               std::ios::binary);
        titlePng.write(
            reinterpret_cast<const char*>(transparentTitlePng.data()),
            static_cast<std::streamsize>(transparentTitlePng.size()));
        titlePng.close();
        writeSolid("cache/generated/caption.png", 0, 0, 255);
        std::ofstream quadrants(root_ / "media/quadrants.ppm",
                                std::ios::binary);
        quadrants << "P6\n4 4\n255\n";
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const bool right = x >= 2;
                const bool bottom = y >= 2;
                const auto level = static_cast<std::uint8_t>(
                    !bottom && !right ? 48
                    : !bottom && right ? 104
                    : bottom && !right ? 168
                                            : 232);
                const std::array<std::uint8_t, 3> color{level, level, level};
                for (const auto channel : color) {
                    quadrants.put(static_cast<char>(channel));
                }
            }
        }
        std::ofstream malformed(root_ / "media/malformed.bin",
                                std::ios::binary);
        malformed << "not a media container";
        writeMonoWave(root_ / "media/microphone.wav", 1000);
        writeMonoWave(root_ / "media/system.wav", 2000);
        writeMonoWave(root_ / "media/fractional.wav", 1000, 48'048);
        writeNonFiniteFloatWave(root_ / "media/nonfinite.wav");
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

domain::MediaAsset imageAsset(std::string id, std::string path) {
    const auto rate = core::FrameRate::create(30, 1).value();
    return domain::MediaAsset::create(
               domain::AssetId::create(std::move(id)).value(),
               domain::MediaKind::Image, std::move(path),
               core::DurationNs{1'000'000'000},
               domain::VideoAssetMetadata{.width = 4, .height = 2,
                                          .frameRate = rate},
               std::nullopt, 100, "fixture",
               domain::AssetAvailability::Available)
        .value();
}

domain::TimeRange oneSecond() {
    return domain::TimeRange::create(core::TimestampNs{},
                                     core::DurationNs{1'000'000'000})
        .value();
}

edit_engine::TimelineSnapshot transformedSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto blue = imageAsset("blue-pip", "media/blue-wide.ppm");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("pip").value(), "PIP", rate)
                        .value();
    const auto track = domain::TrackId::create("video").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    auto pip = domain::VisualTransform::create(
                   0.5, 0.0, 0.5, 1.0, 1.0, 1.0, 0.0,
                   0.0, 0.0, 0.0, 0.0, 0.5, 0)
                   .value();
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("blue-pip").value(),
                                blue, oneSecond(), oneSecond(), true, pip,
                                std::nullopt)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(20).value(),
            {std::move(blue)}, root, 16, 16};
}

edit_engine::TimelineSnapshot zOrderedSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto red = imageAsset("red-z", "media/red-wide.ppm");
    auto blue = imageAsset("blue-z", "media/blue-wide.ppm");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("z-order").value(),
                        "Z order", rate)
                        .value();
    const auto redTrack = domain::TrackId::create("red-track").value();
    const auto blueTrack = domain::TrackId::create("blue-track").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      redTrack, domain::TrackKind::Video,
                                      "Red", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      blueTrack, domain::TrackKind::Video,
                                      "Blue", true, false)
                                      .value())
                    .hasValue());
    const auto transform = [](std::int32_t zOrder) {
        return domain::VisualTransform::create(
                   0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
                   0.0, 0.0, 0.0, 0.0, 1.0, zOrder)
            .value();
    };
    EXPECT_TRUE(timeline.insertClip(
                            redTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("red-top").value(), red,
                                oneSecond(), oneSecond(), true, transform(10),
                                std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            blueTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("blue-bottom").value(),
                                blue, oneSecond(), oneSecond(), true,
                                transform(-10), std::nullopt)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(21).value(),
            {std::move(red), std::move(blue)}, root, 16, 16};
}

edit_engine::TimelineSnapshot transformedQuadrantsSnapshot(
    const fs::path& root, domain::VisualTransform transform,
    std::int64_t revision) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto quadrants = imageAsset("quadrants", "media/quadrants.ppm");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("quadrants").value(),
                        "Quadrants", rate)
                        .value();
    const auto track = domain::TrackId::create("video").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("quadrants").value(),
                                quadrants, oneSecond(), oneSecond(), true,
                                transform, std::nullopt)
                                .value())
                    .hasValue());
    return {std::move(timeline),
            domain::TimelineRevision::create(revision).value(),
            {std::move(quadrants)}, root, 16, 16};
}

edit_engine::TimelineSnapshot generatedSnapshot(const fs::path& root,
                                                 bool omitTitleRaster) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto red = imageAsset("red-base", "media/red-wide.ppm");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("generated").value(),
                        "Generated", rate)
                        .value();
    const auto videoTrack = domain::TrackId::create("video").value();
    const auto titleTrack = domain::TrackId::create("title-1").value();
    const auto captionTrack = domain::TrackId::create("caption-1").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      videoTrack, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      titleTrack, domain::TrackKind::Title,
                                      "Titles", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      captionTrack, domain::TrackKind::Caption,
                                      "Captions", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            videoTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("red-base").value(), red,
                                oneSecond(), oneSecond(), true, std::nullopt,
                                std::nullopt)
                                .value())
                    .hasValue());
    const auto titleRange = domain::TimeRange::create(
                                core::TimestampNs{},
                                core::DurationNs{400'000'000})
                                .value();
    auto title = domain::TitlePayload::create(
                     "한글 제목", "Arial", 0.5, 0.5,
                     domain::RgbaColor::parse("#ffffffff").value(),
                     domain::RgbaColor::parse("#00000000").value(),
                     domain::TextAlignment::Center)
                     .value();
    EXPECT_TRUE(timeline.insertClip(
                            titleTrack,
                            domain::Clip::createTitle(
                                domain::ClipId::create("korean-title").value(),
                                titleRange, true, title, std::nullopt)
                                .value())
                    .hasValue());
    const auto captionCue = domain::CaptionCue::create(
                                domain::CueId::create("korean-cue").value(),
                                core::DurationNs{500'000'000},
                                core::DurationNs{200'000'000}, "한글 자막")
                                .value();
    EXPECT_TRUE(timeline.insertClip(
                            captionTrack,
                            domain::Clip::createCaption(
                                domain::ClipId::create("korean-caption").value(),
                                oneSecond(), true, {captionCue}, std::nullopt)
                                .value())
                    .hasValue());
    std::vector<edit_engine::GeneratedOverlayDescriptor> descriptors;
    if (!omitTitleRaster) {
        descriptors.push_back(
            edit_engine::GeneratedOverlayDescriptor::create(
                domain::ClipId::create("korean-title").value(), std::nullopt,
                "cache/generated/title.png", titleRange, "Arial")
                .value());
    }
    descriptors.push_back(edit_engine::GeneratedOverlayDescriptor::create(
                              domain::ClipId::create("korean-caption").value(),
                              captionCue.id(), "cache/generated/caption.png",
                              domain::TimeRange::create(
                                  core::TimestampNs{
                                      core::DurationNs{500'000'000}},
                                  core::DurationNs{200'000'000})
                                  .value(),
                              "Arial")
                              .value());
    return {std::move(timeline), domain::TimelineRevision::create(22).value(),
            {std::move(red)}, root, 16, 16, std::move(descriptors)};
}

edit_engine::TimelineSnapshot transparentImageAssetSnapshot(
    const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto red = imageAsset("red-background", "media/red-wide.ppm");
    auto overlay = imageAsset("transparent-overlay",
                              "cache/generated/title.png");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("transparent-asset").value(),
                        "Transparent asset", rate)
                        .value();
    const auto backgroundTrack =
        domain::TrackId::create("background").value();
    const auto overlayTrack = domain::TrackId::create("overlay").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      backgroundTrack,
                                      domain::TrackKind::Video,
                                      "Background", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      overlayTrack,
                                      domain::TrackKind::Video,
                                      "Overlay", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            backgroundTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("background").value(),
                                red, oneSecond(), oneSecond(), true,
                                std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            overlayTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("overlay").value(),
                                overlay, oneSecond(), oneSecond(), true,
                                std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(29).value(),
            {std::move(red), std::move(overlay)}, root, 16, 16};
}

edit_engine::TimelineSnapshot envelopedAudioSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto audio = domain::MediaAsset::create(
                     domain::AssetId::create("tone").value(),
                     domain::MediaKind::Audio, "media/microphone.wav",
                     core::DurationNs{1'000'000'000}, std::nullopt,
                     domain::AudioAssetMetadata{48'000, 1}, 96'044,
                     "tone", domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("envelope").value(),
                        "Envelope", rate)
                        .value();
    const auto track = domain::TrackId::create("audio").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Audio,
                                      "Audio", true, false)
                                      .value())
                    .hasValue());
    const auto envelope = domain::AudioEnvelope::create(
                              -6.0, core::DurationNs{200'000'000},
                              core::DurationNs{200'000'000},
                              oneSecond().duration())
                              .value();
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("tone-clip").value(),
                                audio, oneSecond(), oneSecond(), true,
                                std::nullopt, envelope)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(23).value(),
            {std::move(audio)}, root, 16, 16};
}

edit_engine::TimelineSnapshot filteredSnapshot(const fs::path& root,
                                                std::int64_t revision,
                                                bool malformedVisual = false) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto visual = imageAsset("filtered-visual",
                             malformedVisual ? "media/malformed.bin"
                                             : "media/blue-wide.ppm");
    auto audio = domain::MediaAsset::create(
                     domain::AssetId::create("filtered-audio").value(),
                     domain::MediaKind::Audio, "media/microphone.wav",
                     core::DurationNs{1'000'000'000}, std::nullopt,
                     domain::AudioAssetMetadata{48'000, 1}, 96'044,
                     "filtered-audio",
                     domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("filtered").value(),
                        "Filtered", rate)
                        .value();
    const auto videoTrack = domain::TrackId::create("video").value();
    const auto audioTrack = domain::TrackId::create("audio").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      videoTrack, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      audioTrack, domain::TrackKind::Audio,
                                      "Audio", true, false)
                                      .value())
                    .hasValue());
    const auto transform = domain::VisualTransform::create(
                               0.5, 0.0, 0.5, 1.0, 1.0, 1.0, 0.0,
                               0.0, 0.0, 0.0, 0.0, 0.5, 1)
                               .value();
    const auto envelope = domain::AudioEnvelope::create(
                              -6.0, core::DurationNs{200'000'000},
                              core::DurationNs{200'000'000},
                              oneSecond().duration())
                              .value();
    EXPECT_TRUE(timeline.insertClip(
                            videoTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("visual").value(),
                                visual, oneSecond(), oneSecond(), true,
                                transform, std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            audioTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("audio").value(), audio,
                                oneSecond(), oneSecond(), true, std::nullopt,
                                envelope)
                                .value())
                    .hasValue());
    return {std::move(timeline),
            domain::TimelineRevision::create(revision).value(),
            {std::move(visual), std::move(audio)}, root, 16, 16};
}

edit_engine::TimelineSnapshot embeddedVideoAudioSnapshot(
    const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto asset = domain::MediaAsset::create(
                     domain::AssetId::create("av-clip").value(),
                     domain::MediaKind::Video, "media/microphone.wav",
                     core::DurationNs{1'000'000'000},
                     domain::VideoAssetMetadata{16, 16, rate},
                     domain::AudioAssetMetadata{48'000, 1}, 96'044,
                     "av-fixture", domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("embedded-audio").value(),
                        "Embedded audio", rate)
                        .value();
    const auto track = domain::TrackId::create("video").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    const auto envelope = domain::AudioEnvelope::create(
                              -6.0, core::DurationNs{}, core::DurationNs{},
                              oneSecond().duration())
                              .value();
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("av-clip").value(),
                                asset, oneSecond(), oneSecond(), true,
                                std::nullopt, envelope)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(26).value(),
            {std::move(asset)}, root, 16, 16};
}

edit_engine::TimelineSnapshot fractionalAudioSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30'000, 1'001).value();
    const auto duration = core::DurationNs{1'001'000'000};
    auto asset = domain::MediaAsset::create(
                     domain::AssetId::create("fractional").value(),
                     domain::MediaKind::Audio, "media/fractional.wav", duration,
                     std::nullopt, domain::AudioAssetMetadata{48'000, 1},
                     96'140, "fractional",
                     domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("fractional").value(),
                        "Fractional", rate)
                        .value();
    const auto track = domain::TrackId::create("audio").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Audio,
                                      "Audio", true, false)
                                      .value())
                    .hasValue());
    const auto range = domain::TimeRange::create(core::TimestampNs{}, duration)
                           .value();
    const auto envelope = domain::AudioEnvelope::create(
                              0.0, core::DurationNs{400'000'000},
                              core::DurationNs{400'000'000}, duration)
                              .value();
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("fractional").value(),
                                asset, range, range, true, std::nullopt,
                                envelope)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(27).value(),
            {std::move(asset)}, root, 16, 16};
}

edit_engine::TimelineSnapshot nonFiniteAudioSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto asset = domain::MediaAsset::create(
                     domain::AssetId::create("nonfinite").value(),
                     domain::MediaKind::Audio, "media/nonfinite.wav",
                     core::DurationNs{1'000'000'000}, std::nullopt,
                     domain::AudioAssetMetadata{48'000, 1}, 192'044,
                     "nonfinite", domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("nonfinite").value(),
                        "Non-finite", rate)
                        .value();
    const auto track = domain::TrackId::create("audio").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Audio,
                                      "Audio", true, false)
                                      .value())
                    .hasValue());
    const auto envelope = domain::AudioEnvelope::create(
                              0.0, core::DurationNs{}, core::DurationNs{},
                              oneSecond().duration())
                              .value();
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("nonfinite").value(),
                                asset, oneSecond(), oneSecond(), true,
                                std::nullopt, envelope)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(29).value(),
            {std::move(asset)}, root, 16, 16};
}

edit_engine::TimelineSnapshot oversizedSourceSnapshot(const fs::path& root) {
    const auto rate = core::FrameRate::create(30, 1).value();
    const auto firstFrame =
        static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1;
    const auto sourceStart = core::frameToTimestamp(firstFrame, rate);
    const auto duration = core::DurationNs{1'000'000'000};
    auto asset = domain::MediaAsset::create(
                     domain::AssetId::create("oversized-source").value(),
                     domain::MediaKind::Video, "media/red-wide.ppm",
                     sourceStart.time_since_epoch() + duration,
                     domain::VideoAssetMetadata{4, 2, rate}, std::nullopt, 35,
                     "oversized", domain::AssetAvailability::Available)
                     .value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("oversized-source").value(),
                        "Oversized source", rate)
                        .value();
    const auto track = domain::TrackId::create("video").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      track, domain::TrackKind::Video,
                                      "Video", true, false)
                                      .value())
                    .hasValue());
    const auto source = domain::TimeRange::create(sourceStart, duration).value();
    EXPECT_TRUE(timeline.insertClip(
                            track,
                            domain::Clip::createAsset(
                                domain::ClipId::create("oversized").value(),
                                asset, source, oneSecond(), true, std::nullopt,
                                std::nullopt)
                                .value())
                    .hasValue());
    return {std::move(timeline), domain::TimelineRevision::create(28).value(),
            {std::move(asset)}, root, 16, 16};
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

TEST(MltEditEngineTest, EmptyTimelineReturnsDeterministicBlackFrame) {
    MltFixture fixture;
    const auto rate = core::FrameRate::create(30, 1).value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("empty").value(), "Empty",
                        rate)
                        .value();
    edit_engine::TimelineSnapshot empty{
        std::move(timeline), domain::TimelineRevision::create(8).value(), {},
        fixture.root()};
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};

    ASSERT_TRUE(engine.load(empty).hasValue());
    auto frame = engine.requestFrame(core::TimestampNs{});

    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* bgra = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(bgra, nullptr);
    EXPECT_LT(bgra[0], 8U);
    EXPECT_LT(bgra[1], 8U);
    EXPECT_LT(bgra[2], 8U);
    EXPECT_EQ(bgra[3], 255U);
    EXPECT_EQ(frame.value().revision().value(), 8);
}

TEST(MltEditEngineTest, CompositesUpperVideoTrackOverLowerTrack) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(layeredSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    const auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue()) << diagnostics.error().message();
    // Two independent timeline tracks (the merged-recording shape) must
    // assemble exactly two media producers, not one producer per source
    // segment.  The preview background is intentionally excluded.
    EXPECT_EQ(diagnostics.value().mediaProducerCount, 2U);

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
        .previewWidth = 16,
        .previewHeight = 16}};
    ASSERT_TRUE(engine.load(filteredSnapshot(fixture.root(), 3)).hasValue());
    auto beforeFrame = engine.requestFrame(core::TimestampNs{});
    auto beforeAudio = engine.requestMixedAudio(
        core::TimestampNs{core::DurationNs{500'000'000}}, 48'000, 1, 1'600);
    ASSERT_TRUE(beforeFrame.hasValue()) << beforeFrame.error().message();
    ASSERT_TRUE(beforeAudio.hasValue()) << beforeAudio.error().message();
    auto malformed = filteredSnapshot(fixture.root(), 4, true);
    auto change = edit_engine::TimelineChangeSet::create(
                      domain::TimelineRevision::create(3).value(),
                      std::move(malformed),
                      {domain::TrackId::create("video").value()}, true)
                      .value();

    auto updated = engine.update(change);

    EXPECT_FALSE(updated.hasValue());
    auto frame = engine.requestFrame(core::TimestampNs{});
    auto audio = engine.requestMixedAudio(
        core::TimestampNs{core::DurationNs{500'000'000}}, 48'000, 1, 1'600);
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    ASSERT_TRUE(audio.hasValue()) << audio.error().message();
    EXPECT_EQ(frame.value().revision().value(), 3);
    EXPECT_EQ(audio.value(), beforeAudio.value());
    const auto* before = static_cast<const std::uint8_t*>(
        beforeFrame.value().frame().platformHandle.get());
    const auto* after = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(before, nullptr);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(std::equal(before, before + 16U * 16U * 4U, after));
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
    EXPECT_EQ(diagnostics.value().audioMixTransitions, 1U);
    auto mixed = engine.requestMixedAudio(
        core::TimestampNs{}, 48'000, 1, 1'600);
    ASSERT_TRUE(mixed.hasValue()) << mixed.error().message();
    ASSERT_EQ(mixed.value().size(), 1'600U);
    double mean = 0.0;
    for (const float sample : mixed.value()) mean += sample;
    mean /= static_cast<double>(mixed.value().size());
    const auto [minimum, maximum] =
        std::minmax_element(mixed.value().begin(), mixed.value().end());
    EXPECT_GT(mean, 0.08) << "first=" << mixed.value().front()
                          << " min=" << *minimum << " max=" << *maximum;
    EXPECT_LT(mean, 0.10);
    // Audio extraction consumes this graph position; video is covered by the
    // dedicated composite tests.
}

TEST(MltEditEngineTest, RepeatedEngineDestructionDoesNotCorruptNextGraph) {
    MltFixture fixture;
    for (int iteration = 0; iteration < 3; ++iteration) {
        mlt_adapter::MltEditEngine engine{{
            .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
            .previewWidth = 16,
            .previewHeight = 16}};
        auto loaded =
            engine.load(filteredSnapshot(fixture.root(), iteration + 10));
        ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
        auto frame = engine.requestFrame(core::TimestampNs{});
        ASSERT_TRUE(frame.hasValue()) << frame.error().message();
        EXPECT_EQ(frame.value().revision().value(), iteration + 10);
        const auto* bgra = static_cast<const std::uint8_t*>(
            frame.value().frame().platformHandle.get());
        ASSERT_NE(bgra, nullptr);
        EXPECT_GT(bgra[(8U * 16U + 12U) * 4U], 100U);
        auto audio = engine.requestMixedAudio(
            core::TimestampNs{core::DurationNs{500'000'000}},
            48'000, 1, 1'600);
        ASSERT_TRUE(audio.hasValue()) << audio.error().message();
        EXPECT_GT(audio.value().front(), 0.01F);
    }
}

TEST(MltEditEngineTest, RendersPipOpacityWithTransparentPadding) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    auto loaded = engine.load(transformedSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto frame = engine.requestFrame(core::TimestampNs{});

    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixels, nullptr);
    const auto left = (8U * 16U + 4U) * 4U;
    const auto right = (8U * 16U + 12U) * 4U;
    EXPECT_LT(pixels[left], 16U);
    EXPECT_LT(pixels[left + 1U], 16U);
    EXPECT_LT(pixels[left + 2U], 16U);
    EXPECT_GT(pixels[right], 100U);
    EXPECT_LT(pixels[right], 160U);
    // The audited core compositor round-trips through 4:2:2 chroma, so allow
    // a small non-blue residue at the hard alpha edge.
    EXPECT_LT(pixels[right + 1U], 32U);
    EXPECT_LT(pixels[right + 2U], 32U);
    auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue());
    EXPECT_EQ(diagnostics.value().visualBranchCount, 1U);
    EXPECT_EQ(diagnostics.value().transformedVisualBranchCount, 1U);
}

TEST(MltEditEngineTest, CompositesVisualBranchesByZOrderNotTrackPosition) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(zOrderedSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto frame = engine.requestFrame(core::TimestampNs{});

    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixels, nullptr);
    EXPECT_LT(pixels[0], 16U);
    EXPECT_LT(pixels[1], 16U);
    EXPECT_GT(pixels[2], 239U);
}

TEST(MltEditEngineTest, AppliesPhysicalCropBeforeComposite) {
    MltFixture fixture;
    const auto crop = domain::VisualTransform::create(
                          0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
                          0.5, 0.0, 0.0, 0.0, 1.0, 0)
                          .value();
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    auto loaded = engine.load(
        transformedQuadrantsSnapshot(fixture.root(), crop, 24));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto frame = engine.requestFrame(core::TimestampNs{});

    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixels, nullptr);
    const auto top = (4U * 16U + 4U) * 4U;
    const auto bottom = (12U * 16U + 4U) * 4U;
    EXPECT_GT(pixels[top], 80U);
    EXPECT_LT(pixels[top], 130U);
    EXPECT_GT(pixels[bottom], 205U);
}

TEST(MltEditEngineTest, AppliesPhysicalQuarterTurnByInverseMapping) {
    MltFixture fixture;
    const auto rotation = domain::VisualTransform::create(
                              0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 90.0,
                              0.0, 0.0, 0.0, 0.0, 1.0, 0)
                              .value();
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    auto loaded = engine.load(
        transformedQuadrantsSnapshot(fixture.root(), rotation, 25));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto frame = engine.requestFrame(core::TimestampNs{});

    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixels, nullptr);
    const auto topLeft = (4U * 16U + 4U) * 4U;
    const auto topRight = (4U * 16U + 12U) * 4U;
    const auto bottomLeft = (12U * 16U + 4U) * 4U;
    const auto bottomRight = (12U * 16U + 12U) * 4U;
    EXPECT_GT(pixels[topLeft], 140U);
    EXPECT_LT(pixels[topLeft], 195U);
    EXPECT_GT(pixels[topRight], 25U);
    EXPECT_LT(pixels[topRight], 70U);
    EXPECT_GT(pixels[bottomLeft], 205U);
    EXPECT_GT(pixels[bottomRight], 80U);
    EXPECT_LT(pixels[bottomRight], 130U);
}

TEST(MltEditEngineTest, ShowsGeneratedKoreanTitleAndCaptionOnlyInRange) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(generatedSnapshot(fixture.root(), false));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto title = engine.requestFrame(core::TimestampNs{
        core::DurationNs{100'000'000}});
    auto caption = engine.requestFrame(core::TimestampNs{
        core::DurationNs{600'000'000}});
    auto base = engine.requestFrame(core::TimestampNs{
        core::DurationNs{800'000'000}});

    ASSERT_TRUE(title.hasValue()) << title.error().message();
    ASSERT_TRUE(caption.hasValue()) << caption.error().message();
    ASSERT_TRUE(base.hasValue()) << base.error().message();
    const auto* titlePixel = static_cast<const std::uint8_t*>(
        title.value().frame().platformHandle.get());
    const auto* captionPixel = static_cast<const std::uint8_t*>(
        caption.value().frame().platformHandle.get());
    const auto* basePixel = static_cast<const std::uint8_t*>(
        base.value().frame().platformHandle.get());
    ASSERT_NE(titlePixel, nullptr);
    ASSERT_NE(captionPixel, nullptr);
    ASSERT_NE(basePixel, nullptr);
    EXPECT_GT(titlePixel[2], 239U);
    EXPECT_LT(titlePixel[1], 16U);
    constexpr std::size_t titleGlyphPixel = 3U * 4U;
    EXPECT_GT(titlePixel[titleGlyphPixel + 1U], 200U);
    EXPECT_LT(titlePixel[titleGlyphPixel], 32U);
    EXPECT_LT(titlePixel[titleGlyphPixel + 2U], 32U);
    EXPECT_GT(captionPixel[0], 239U);
    EXPECT_LT(captionPixel[2], 16U);
    EXPECT_GT(basePixel[2], 239U);
    auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue());
    EXPECT_EQ(diagnostics.value().visualBranchCount, 3U);
    EXPECT_EQ(diagnostics.value().missingOverlayCount, 0U);
}

TEST(MltEditEngineTest, PreservesAlphaForIdentityTransparentImageAsset) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(transparentImageAssetSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto frame = engine.requestFrame(core::TimestampNs{});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixels, nullptr);
    EXPECT_GT(pixels[2], 239U);
    EXPECT_LT(pixels[1], 32U);
    constexpr std::size_t glyphPixel = 3U * 4U;
    EXPECT_GT(pixels[glyphPixel + 1U], 200U);
    EXPECT_LT(pixels[glyphPixel], 32U);
    EXPECT_LT(pixels[glyphPixel + 2U], 32U);
}

TEST(MltEditEngineTest, MissingGeneratedRasterStaysTransparentAndDiagnosed) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(generatedSnapshot(fixture.root(), true));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto frame = engine.requestFrame(core::TimestampNs{
        core::DurationNs{100'000'000}});

    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixel = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixel, nullptr);
    EXPECT_LT(pixel[0], 16U);
    EXPECT_LT(pixel[1], 16U);
    EXPECT_GT(pixel[2], 239U);
    auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue());
    EXPECT_EQ(diagnostics.value().missingOverlayCount, 1U);
}

TEST(MltEditEngineTest, AppliesRealPcmGainAndClipLocalFades) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 4,
        .previewHeight = 2}};
    auto loaded = engine.load(envelopedAudioSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    const auto meanAbsolute = [](const std::vector<float>& samples) {
        double total = 0.0;
        for (const auto sample : samples) total += std::abs(sample);
        return total / static_cast<double>(samples.size());
    };

    auto start = engine.requestMixedAudio(core::TimestampNs{}, 48'000, 1, 1'600);
    auto middle = engine.requestMixedAudio(
        core::TimestampNs{core::DurationNs{500'000'000}}, 48'000, 1, 1'600);
    auto ending = engine.requestMixedAudio(
        core::TimestampNs{core::DurationNs{900'000'000}}, 48'000, 1, 1'600);

    ASSERT_TRUE(start.hasValue()) << start.error().message();
    ASSERT_TRUE(middle.hasValue()) << middle.error().message();
    ASSERT_TRUE(ending.hasValue()) << ending.error().message();
    const double startMean = meanAbsolute(start.value());
    const double middleMean = meanAbsolute(middle.value());
    const double endingMean = meanAbsolute(ending.value());
    EXPECT_NEAR(middleMean, (1000.0 / 32768.0) * 0.5011872336, 0.001);
    EXPECT_LT(startMean, middleMean * 0.2);
    EXPECT_GT(endingMean, startMean * 2.0);
    EXPECT_LT(endingMean, middleMean * 0.6);
    auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue());
    EXPECT_EQ(diagnostics.value().audioEnvelopeBranchCount, 1U);
}

TEST(MltEditEngineTest, AppliesEnvelopeToAudioCarriedByVideoClip) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    auto loaded = engine.load(embeddedVideoAudioSnapshot(fixture.root()));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();

    auto audio = engine.requestMixedAudio(
        core::TimestampNs{core::DurationNs{500'000'000}}, 48'000, 1, 1'600);

    ASSERT_TRUE(audio.hasValue()) << audio.error().message();
    ASSERT_EQ(audio.value().size(), 1'600U);
    EXPECT_NEAR(audio.value().front(),
                (1000.0F / 32768.0F) * 0.5011872F, 0.0001F);
    auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue());
    EXPECT_EQ(diagnostics.value().audioEnvelopeBranchCount, 1U);
}

TEST(MltEditEngineTest, RejectsSourcePositionsOutsideMltIntegerRange) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};

    auto loaded = engine.load(oversizedSourceSnapshot(fixture.root()));

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_EQ(loaded.error().message(),
              "visual source position exceeds the MLT frame range");
}

TEST(MltEditEngineTest, ReportsAudioProcessorFailureInsteadOfReturningSilence) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    ASSERT_TRUE(engine.load(nonFiniteAudioSnapshot(fixture.root())).hasValue());

    auto audio = engine.requestMixedAudio(core::TimestampNs{}, 48'000, 1, 1'600);

    ASSERT_FALSE(audio.hasValue());
    EXPECT_NE(audio.error().message().find("Creator audio processor failed"),
              std::string::npos);
}

TEST(MltEditEngineTest, UsesExactFractionalRateSamplePositionForFade) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    const auto rate = core::FrameRate::create(30'000, 1'001).value();
    ASSERT_TRUE(engine.load(fractionalAudioSnapshot(fixture.root())).hasValue());
    const auto position = core::frameToTimestamp(10, rate);

    auto audio = engine.requestMixedAudio(position, 48'000, 1, 1'602);

    ASSERT_TRUE(audio.hasValue()) << audio.error().message();
    ASSERT_EQ(audio.value().size(), 1'602U);
    constexpr std::uint64_t firstSample = 16'016;
    constexpr std::uint64_t fadeSamples = 19'200;
    const auto expected = [](std::uint64_t sample) {
        return (1000.0F / 32768.0F) *
               (static_cast<float>(sample) /
                static_cast<float>(fadeSamples - 1U));
    };
    EXPECT_NEAR(audio.value().front(), expected(firstSample), 0.00002F);
    EXPECT_NEAR(audio.value()[99], expected(firstSample + 99U), 0.00002F);
}

TEST(MltEditEngineTest, RendersFrozenTimelineToValidatedH264AacMp4) {
    MltFixture fixture;
    auto lifecycle = std::make_shared<PublicationBoundaryLifecycle>();
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2,
        .renderLifecycle = lifecycle}};
    auto frozen = snapshot(fixture.root(), 31);
    ASSERT_TRUE(engine.load(frozen).hasValue());
    const auto destination = fixture.root() / "export.mp4";
    auto request = edit_engine::RenderRequest::create(
        domain::ProjectId::create("project").value(), frozen, destination,
        edit_engine::RenderPreset::h2641080p30().value(),
        edit_engine::RenderOverwritePolicy::FailIfExists);
    ASSERT_TRUE(request.hasValue()) << request.error().message();

    auto job = engine.render(request.value());
    ASSERT_TRUE(job.hasValue()) << job.error().message();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{30};
    edit_engine::RenderJobState state = edit_engine::RenderJobState::Pending;
    while (std::chrono::steady_clock::now() < deadline) {
        auto progress = job.value()->progress();
        ASSERT_TRUE(progress.hasValue());
        state = progress.value().state();
        if (state == edit_engine::RenderJobState::Completed ||
            state == edit_engine::RenderJobState::Failed ||
            state == edit_engine::RenderJobState::Cancelled) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(state, edit_engine::RenderJobState::Completed);
    ASSERT_TRUE(fs::exists(destination));
    ffmpeg_adapter::FfmpegMediaProbe probe;
    auto media = probe.probe(fixture.root(), destination.filename());
    ASSERT_TRUE(media.hasValue()) << media.error().message();
    EXPECT_EQ(media.value().codecName, "h264");
    ASSERT_TRUE(media.value().video.has_value());
    EXPECT_EQ(media.value().video->width, 1920);
    EXPECT_EQ(media.value().video->height, 1080);
    ASSERT_TRUE(media.value().audio.has_value());
    EXPECT_EQ(media.value().audio->sampleRate, 48'000);
    EXPECT_EQ(media.value().audio->channels, 2);
    EXPECT_TRUE(lifecycle->began.load());
    EXPECT_TRUE(lifecycle->selected.load());
    EXPECT_TRUE(lifecycle->ran.load());
    EXPECT_TRUE(lifecycle->preparedBeforeRename.load());
    EXPECT_TRUE(lifecycle->completedAfterRename.load());
}

TEST(MltEditEngineTest, ImmediateCancellationPublishesNoDestinationOrPartial) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};
    auto frozen = snapshot(fixture.root(), 32);
    const auto destination = fixture.root() / "cancelled.mp4";
    auto request = edit_engine::RenderRequest::create(
        domain::ProjectId::create("project").value(), frozen, destination,
        edit_engine::RenderPreset::h2641080p30().value(),
        edit_engine::RenderOverwritePolicy::FailIfExists);
    ASSERT_TRUE(request.hasValue());
    auto job = engine.render(request.value());
    ASSERT_TRUE(job.hasValue()) << job.error().message();
    ASSERT_TRUE(job.value()->cancel().hasValue());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{15};
    edit_engine::RenderJobState state = edit_engine::RenderJobState::Pending;
    while (std::chrono::steady_clock::now() < deadline) {
        state = job.value()->progress().value().state();
        if (state == edit_engine::RenderJobState::Cancelled) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_EQ(state, edit_engine::RenderJobState::Cancelled);
    EXPECT_FALSE(fs::exists(destination));
    std::size_t partialCount = 0;
    for (const auto& entry : fs::directory_iterator(fixture.root())) {
        if (entry.path().filename().string().find(".partial.mp4") !=
            std::string::npos) {
            ++partialCount;
        }
    }
    EXPECT_EQ(partialCount, 0U);
}

TEST(MltEditEngineTest, RendersTwoMixedAudioTracksWithoutInvalidSamples) {
    MltFixture fixture;
    mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_MLT_ROOT},
        .previewWidth = 2,
        .previewHeight = 2}};
    auto frozen = audioMixSnapshot(fixture.root());
    const auto destination = fixture.root() / "mixed-audio.mp4";
    auto request = edit_engine::RenderRequest::create(
        domain::ProjectId::create("project").value(), frozen, destination,
        edit_engine::RenderPreset::h2641080p30().value(),
        edit_engine::RenderOverwritePolicy::FailIfExists);
    ASSERT_TRUE(request.hasValue());
    auto job = engine.render(request.value());
    ASSERT_TRUE(job.hasValue()) << job.error().message();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{30};
    edit_engine::RenderJobState state = edit_engine::RenderJobState::Pending;
    while (std::chrono::steady_clock::now() < deadline) {
        state = job.value()->progress().value().state();
        if (state == edit_engine::RenderJobState::Completed ||
            state == edit_engine::RenderJobState::Failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(state, edit_engine::RenderJobState::Completed);
    ffmpeg_adapter::FfmpegMediaProbe probe;
    auto media = probe.probe(fixture.root(), destination.filename());
    ASSERT_TRUE(media.hasValue()) << media.error().message();
    ASSERT_TRUE(media.value().audio.has_value());
    EXPECT_EQ(media.value().audio->sampleRate, 48'000);
    EXPECT_EQ(media.value().audio->channels, 2);
}

}  // namespace
