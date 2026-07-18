#include "app/ProjectExportEngine.h"

#include "core/Uuid.h"
#include "domain/Timeline.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteRenderJobStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace creator;
namespace fs = std::filesystem;

void writePpm(const fs::path& path, std::uint8_t red, std::uint8_t green,
              std::uint8_t blue) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n4 2\n255\n";
    for (int pixel = 0; pixel < 8; ++pixel) {
        output.put(static_cast<char>(red));
        output.put(static_cast<char>(green));
        output.put(static_cast<char>(blue));
    }
}

void writeWave(const fs::path& path, std::int16_t sampleValue) {
    constexpr std::uint32_t sampleRate = 48'000;
    constexpr std::uint32_t dataBytes = sampleRate * sizeof(std::int16_t);
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
    write16(1);
    write32(sampleRate);
    write32(sampleRate * sizeof(std::int16_t));
    write16(sizeof(std::int16_t));
    write16(16);
    output.write("data", 4);
    write32(dataBytes);
    for (std::uint32_t sample = 0; sample < sampleRate; ++sample) {
        write16(static_cast<std::uint16_t>(sampleValue));
    }
}

TEST(ProjectExportEngineIntegrationTest,
     PersistsAndPublishesValidatedProjectMp4EndToEnd) {
    std::u8string rootName{u8"creator-studio-내보내기-"};
    for (const char value : core::generateUuidV4()) {
        rootName.push_back(static_cast<char8_t>(value));
    }
    const auto root = fs::temp_directory_path() / fs::path{rootName};
    const auto packageRoot = root / fs::path{u8"강의-프로젝트.cstudio"};
    fs::create_directories(root);
    struct Cleanup final {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};

    project_store::ProjectPackageStore packages;
    auto created = packages.create(packageRoot, "Export acceptance");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    const auto projectId = created.value().package.manifest.projectId;
    fs::create_directories(packageRoot / "media");
    fs::create_directories(packageRoot / "audio");
    fs::create_directories(packageRoot / "cache/generated");
    writePpm(packageRoot / "media/screen.ppm", 220, 24, 24);
    writePpm(packageRoot / "media/camera.ppm", 24, 24, 220);
    writePpm(packageRoot / "cache/generated/title.ppm", 24, 220, 24);
    writePpm(packageRoot / "cache/generated/caption.ppm", 220, 220, 24);
    writeWave(packageRoot / "audio/microphone.wav", 1'000);
    writeWave(packageRoot / "audio/system.wav", 2'000);
    const auto rate = core::FrameRate::create(30, 1).value();
    const auto assetDuration = core::DurationNs{1'000'000'000};
    const auto timelineDuration = core::DurationNs{300'000'000};
    const auto imageAsset = [&](std::string id, std::string path,
                                std::string fingerprint) {
        return domain::MediaAsset::create(
                   domain::AssetId::create(std::move(id)).value(),
                   domain::MediaKind::Image, std::move(path), assetDuration,
                   domain::VideoAssetMetadata{4, 2, rate}, std::nullopt, 35,
                   std::move(fingerprint),
                   domain::AssetAvailability::Available)
            .value();
    };
    const auto audioAsset = [&](std::string id, std::string path,
                                std::string fingerprint) {
        return domain::MediaAsset::create(
                   domain::AssetId::create(std::move(id)).value(),
                   domain::MediaKind::Audio, std::move(path), assetDuration,
                   std::nullopt, domain::AudioAssetMetadata{48'000, 1}, 96'044,
                   std::move(fingerprint),
                   domain::AssetAvailability::Available)
            .value();
    };
    auto screen = imageAsset("screen", "media/screen.ppm", "screen-hash");
    auto camera = imageAsset("camera", "media/camera.ppm", "camera-hash");
    auto microphone = audioAsset("microphone", "audio/microphone.wav",
                                 "microphone-hash");
    auto system =
        audioAsset("system", "audio/system.wav", "system-hash");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main",
                        rate)
                        .value();
    const auto screenTrack = domain::TrackId::create("screen-track").value();
    const auto cameraTrack = domain::TrackId::create("camera-track").value();
    const auto microphoneTrack =
        domain::TrackId::create("microphone-track").value();
    const auto systemTrack = domain::TrackId::create("system-track").value();
    const auto titleTrack = domain::TrackId::create("titles").value();
    const auto captionTrack = domain::TrackId::create("captions").value();
    const auto addTrack = [&](const domain::TrackId& id,
                              domain::TrackKind kind, std::string name) {
        ASSERT_TRUE(timeline.addTrack(domain::Track::create(
                                          id, kind, std::move(name), true, false)
                                          .value())
                        .hasValue());
    };
    addTrack(screenTrack, domain::TrackKind::Video, "Screen");
    addTrack(cameraTrack, domain::TrackKind::Video, "Camera");
    addTrack(microphoneTrack, domain::TrackKind::Audio, "Microphone");
    addTrack(systemTrack, domain::TrackKind::Audio, "System");
    addTrack(titleTrack, domain::TrackKind::Title, "Titles");
    addTrack(captionTrack, domain::TrackKind::Caption, "Captions");
    const auto range = domain::TimeRange::create(
                           core::TimestampNs{}, timelineDuration)
                           .value();
    const auto pip = domain::VisualTransform::create(
                         0.66, 0.05, 0.29, 0.29, 1.0, 1.0, 0.0, 0.05, 0.0,
                         0.05, 0.0, 0.95, 1)
                         .value();
    const auto microphoneEnvelope = domain::AudioEnvelope::create(
                                        -3.0,
                                        core::DurationNs{30'000'000},
                                        core::DurationNs{30'000'000},
                                        timelineDuration)
                                        .value();
    const auto systemEnvelope = domain::AudioEnvelope::create(
                                    -9.0, core::DurationNs{},
                                    core::DurationNs{30'000'000},
                                    timelineDuration)
                                    .value();
    ASSERT_TRUE(timeline.insertClip(
                            screenTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("screen-clip").value(),
                                screen, range, range, true, std::nullopt,
                                std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            cameraTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("camera-clip").value(),
                                camera, range, range, true, pip, std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            microphoneTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("microphone-clip").value(),
                                microphone, range, range, true, std::nullopt,
                                microphoneEnvelope)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            systemTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("system-clip").value(),
                                system, range, range, true, std::nullopt,
                                systemEnvelope)
                                .value())
                    .hasValue());
    const auto titleRange = domain::TimeRange::create(
                                core::TimestampNs{},
                                core::DurationNs{60'000'000})
                                .value();
    const auto title = domain::TitlePayload::create(
                           "한글 Export", "Creator Sans", 0.5, 0.5,
                           domain::RgbaColor::parse("#ffffffff").value(),
                           domain::RgbaColor::parse("#00000000").value(),
                           domain::TextAlignment::Center)
                           .value();
    ASSERT_TRUE(timeline.insertClip(
                            titleTrack,
                            domain::Clip::createTitle(
                                domain::ClipId::create("title").value(),
                                titleRange, true, title, std::nullopt)
                                .value())
                    .hasValue());
    const auto cue = domain::CaptionCue::create(
                         domain::CueId::create("caption-cue").value(),
                         core::DurationNs{150'000'000},
                         core::DurationNs{60'000'000}, "정확한 자막")
                         .value();
    ASSERT_TRUE(timeline.insertClip(
                            captionTrack,
                            domain::Clip::createCaption(
                                domain::ClipId::create("caption").value(), range,
                                true, {cue}, std::nullopt)
                                .value())
                    .hasValue());
    auto timelineStore = project_store::SqliteTimelineStore::open(
        created.value().databasePath, projectId);
    ASSERT_TRUE(timelineStore.hasValue()) << timelineStore.error().message();
    ASSERT_TRUE(timelineStore.value().putAsset(screen).hasValue());
    ASSERT_TRUE(timelineStore.value().putAsset(camera).hasValue());
    ASSERT_TRUE(timelineStore.value().putAsset(microphone).hasValue());
    ASSERT_TRUE(timelineStore.value().putAsset(system).hasValue());
    ASSERT_TRUE(timelineStore.value().createTimeline(timeline).hasValue());
    auto persisted = timelineStore.value().loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue()) << persisted.error().message();

    edit_engine::TimelineSnapshot snapshot{
        .timeline = persisted.value().timeline,
        .revision = domain::TimelineRevision::create(
                        persisted.value().revision)
                        .value(),
        .assets = {screen, camera, microphone, system},
        .mediaRoot = packageRoot,
        .generatedOverlays = {
            edit_engine::GeneratedOverlayDescriptor::create(
                domain::ClipId::create("title").value(), std::nullopt,
                "cache/generated/title.ppm", titleRange, "Creator Sans")
                .value(),
            edit_engine::GeneratedOverlayDescriptor::create(
                domain::ClipId::create("caption").value(), cue.id(),
                "cache/generated/caption.ppm",
                domain::TimeRange::create(
                    core::TimestampNs{core::DurationNs{150'000'000}},
                    core::DurationNs{60'000'000})
                    .value(),
                "Creator Sans")
                .value()}};
    app::ProjectExportEngine engine{fs::path{CS_TEST_MLT_ROOT}};
    ffmpeg_adapter::FfmpegMediaProbe probe;
    struct ProductCase final {
        edit_engine::RenderPreset preset;
        fs::path destination;
        int width;
        int height;
    };
    std::vector<ProductCase> cases;
    cases.push_back({edit_engine::RenderPreset::h2641080p30().value(),
                     root / fs::path{u8"완성-1080p.mp4"}, 1920, 1080});
    cases.push_back({edit_engine::RenderPreset::h2642160p30().value(),
                     root / fs::path{u8"완성-4K.mp4"}, 3840, 2160});
    for (const auto& product : cases) {
        auto request = edit_engine::RenderRequest::create(
            projectId, snapshot, product.destination, product.preset,
            edit_engine::RenderOverwritePolicy::FailIfExists);
        ASSERT_TRUE(request.hasValue()) << request.error().message();
        const auto jobId = request.value().jobId();
        auto job = engine.render(request.value());
        ASSERT_TRUE(job.hasValue()) << job.error().message();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds{60};
        edit_engine::RenderJobState state =
            edit_engine::RenderJobState::Pending;
        while (std::chrono::steady_clock::now() < deadline) {
            auto progress = job.value()->progress();
            ASSERT_TRUE(progress.hasValue()) << progress.error().message();
            state = progress.value().state();
            if (state == edit_engine::RenderJobState::Completed ||
                state == edit_engine::RenderJobState::Failed ||
                state == edit_engine::RenderJobState::Cancelled) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ASSERT_EQ(state, edit_engine::RenderJobState::Completed);
        ASSERT_TRUE(fs::is_regular_file(product.destination));

        auto reopened = packages.open(packageRoot);
        ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
        const auto lease = reopened.value().databaseIdentityLease;
        ASSERT_TRUE(lease);
        auto jobs = project_store::SqliteRenderJobStore::open(
            reopened.value().databasePath, projectId,
            [lease] { return lease->verifyCurrentIdentity(); });
        ASSERT_TRUE(jobs.hasValue()) << jobs.error().message();
        auto record = jobs.value().load(jobId);
        ASSERT_TRUE(record.hasValue()) << record.error().message();
        ASSERT_TRUE(record.value().has_value());
        EXPECT_EQ(record.value()->progress.state(),
                  edit_engine::RenderJobState::Completed);
        EXPECT_TRUE(record.value()->diagnostics.selectedEncoder.has_value());
        EXPECT_TRUE(record.value()->diagnostics.outputSha256.has_value());
        EXPECT_TRUE(record.value()->diagnostics.destinationIdentity.has_value());

        auto media = probe.probe(root, product.destination.filename());
        ASSERT_TRUE(media.hasValue()) << media.error().message();
        EXPECT_EQ(media.value().codecName, "h264");
        ASSERT_TRUE(media.value().video.has_value());
        EXPECT_EQ(media.value().video->width, product.width);
        EXPECT_EQ(media.value().video->height, product.height);
        ASSERT_TRUE(media.value().audio.has_value());
        EXPECT_EQ(media.value().audio->sampleRate, 48'000);
        EXPECT_EQ(media.value().audio->channels, 2);
    }
    for (const auto& entry : fs::directory_iterator(root)) {
        EXPECT_EQ(entry.path().filename().string().find(".partial.mp4"),
                  std::string::npos);
    }
}

}  // namespace
