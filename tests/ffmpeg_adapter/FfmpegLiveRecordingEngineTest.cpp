#include "app/FfmpegLiveRecordingEngine.h"

#include "app/ILiveCaptureBindings.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "project_store/ProjectPackageStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using creator::app::FfmpegLiveRecordingEngine;
using creator::app::ILiveCaptureBindings;
using creator::app::LiveCaptureSource;
using creator::app::LiveRecordingCompletion;
using creator::app::LiveRecordingStart;
using creator::core::Result;
using creator::domain::RecordingSession;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::ffmpeg_adapter::CpuBgraFrameBuffer;
using creator::project_store::ProjectPackageStore;
using creator::recorder::TrackRole;

class CaptureBindingsFake final : public ILiveCaptureBindings {
public:
    [[nodiscard]] std::vector<LiveCaptureSource> activeSources() const override {
        return sources;
    }
    [[nodiscard]] Result<void> attach(
        const LiveCaptureSource& source,
        std::shared_ptr<creator::capture::IVideoFrameSink> videoSink,
        std::shared_ptr<creator::capture::IAudioBlockSink> audioSink) override {
        if (source.role == TrackRole::Screen || source.role == TrackRole::Camera) {
            if (!videoSink) {
                return creator::core::AppError{creator::core::ErrorCode::InvalidArgument,
                                               "missing video sink"};
            }
            video = std::move(videoSink);
        } else {
            if (!audioSink) {
                return creator::core::AppError{creator::core::ErrorCode::InvalidArgument,
                                               "missing audio sink"};
            }
            audio = std::move(audioSink);
        }
        return creator::core::ok();
    }
    void detachAll() noexcept override {
        ++detachCalls;
        video.reset();
        audio.reset();
    }
    void dispatch(std::function<void()> work) override { work(); }

    std::vector<LiveCaptureSource> sources;
    std::shared_ptr<creator::capture::IVideoFrameSink> video;
    std::shared_ptr<creator::capture::IAudioBlockSink> audio;
    int detachCalls{0};
};

creator::media::VideoFrame videoFrame(std::uint32_t index) {
    auto buffer = CpuBgraFrameBuffer::create(64, 48).value();
    std::fill_n(buffer->data(), buffer->size(),
                static_cast<std::uint8_t>((index * 7U) % 255U));
    return creator::media::VideoFrame{
        .timestamp = creator::core::TimestampNs{std::chrono::nanoseconds{
            static_cast<std::int64_t>((static_cast<std::uint64_t>(index) *
                                       1'000'000'000ULL) /
                                      30ULL)}},
        .width = 64,
        .height = 48,
        .visibleRect = {0, 0, 64, 48},
        .contentWidth = 64,
        .contentHeight = 48,
        .pixelFormat = creator::media::PixelFormat::Bgra8,
        .platformHandle = std::move(buffer),
    };
}

creator::media::AudioBlock audioBlock(std::uint32_t index) {
    constexpr std::uint32_t frames = 480;
    auto samples = std::shared_ptr<float[]>(new float[frames * 2]);
    for (std::uint32_t sample = 0; sample < frames * 2; ++sample) {
        samples[sample] = static_cast<float>(static_cast<int>(sample % 11) - 5) / 32.0F;
    }
    return creator::media::AudioBlock{
        .timestamp = creator::core::TimestampNs{std::chrono::milliseconds{index * 10ULL}},
        .sampleRate = 48'000,
        .channels = 2,
        .frameCount = frames,
        .samples = std::move(samples),
    };
}

creator::core::Utc utc(std::string_view value) {
    return creator::core::Utc::parseRfc3339(value).value();
}

TEST(FfmpegLiveRecordingEngineTest, RecordsAttachedScreenAndMicrophoneToReadyFiles) {
    const auto root = fs::temp_directory_path() / "cs_ffmpeg_live_engine";
    const auto packagePath = root / "live.cstudio";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    auto store = std::make_shared<ProjectPackageStore>();
    ASSERT_TRUE(store->create(packagePath, "Live engine").hasValue());
    const auto sessionId = SessionId::create("live-session").value();
    ASSERT_TRUE(store->beginRecording(packagePath, sessionId, {},
                                      utc("2026-07-16T11:00:00Z"))
                    .hasValue());
    auto bindings = std::make_shared<CaptureBindingsFake>();
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    bindings->sources = {{screen, TrackRole::Screen},
                         {microphone, TrackRole::Microphone}};
    FfmpegLiveRecordingEngine engine{bindings, store};
    ASSERT_TRUE(engine.available()) << engine.unavailableReason();
    auto promise = std::make_shared<std::promise<Result<LiveRecordingCompletion>>>();
    auto future = promise->get_future();

    const auto engineStarted = engine.start(
                          LiveRecordingStart{.sessionId = sessionId,
                                             .packagePath = packagePath,
                                             .startedAt = {}},
                          [promise](auto result) { promise->set_value(std::move(result)); });
    ASSERT_TRUE(engineStarted.hasValue()) << engineStarted.error().message();
    ASSERT_TRUE(bindings->video);
    ASSERT_TRUE(bindings->audio);
    // Establish the deterministic microphone master before follower video.
    bindings->audio->onAudioBlock(audioBlock(0));
    for (std::uint32_t frame = 0; frame < 63; ++frame) {
        bindings->video->onVideoFrame(videoFrame(frame));
    }
    const auto synchronized = engine.snapshot();
    EXPECT_GT(synchronized.syncVideoFramesDuplicated, 0u);
    EXPECT_GT(synchronized.maximumAbsoluteDriftNanoseconds, 0u);
    for (std::uint32_t block = 1; block < 210; ++block) {
        bindings->audio->onAudioBlock(audioBlock(block));
    }
    engine.stopAsync(creator::core::TimestampNs{std::chrono::milliseconds{2100}});
    const auto completed = future.get();

    ASSERT_TRUE(completed.hasValue()) << completed.error().message();
    EXPECT_EQ(completed.value().trackCount, 2u);
    EXPECT_GE(completed.value().segmentsPublished, 3u);
    EXPECT_EQ(completed.value().session.segmentCount(),
              completed.value().segmentsPublished);
    EXPECT_FALSE(completed.value().terminalError.has_value());
    EXPECT_GE(bindings->detachCalls, 1);
    for (const auto& segment : completed.value().session.segments()) {
        EXPECT_TRUE(fs::is_regular_file(packagePath / fs::path{segment.relativePath}));
        EXPECT_EQ(segment.status, creator::domain::SegmentStatus::Ready);
    }
    const auto hasRolePath = [&completed](std::string_view prefix) {
        return std::any_of(
            completed.value().session.segments().begin(),
            completed.value().session.segments().end(),
            [prefix](const auto& segment) {
                return segment.relativePath.starts_with(prefix);
            });
    };
    EXPECT_TRUE(hasRolePath("media/screen/"));
    EXPECT_TRUE(hasRolePath("audio/microphone/"));
    ASSERT_TRUE(store->completeRecording(packagePath, completed.value().session,
                                         utc("2026-07-16T11:00:03Z"))
                    .hasValue());
    fs::remove_all(root, ignored);
}

TEST(FfmpegLiveRecordingEngineTest, RejectsStartWithoutAnActiveCaptureSource) {
    auto bindings = std::make_shared<CaptureBindingsFake>();
    auto store = std::make_shared<ProjectPackageStore>();
    FfmpegLiveRecordingEngine engine{bindings, store};
    const auto result = engine.start(
        LiveRecordingStart{.sessionId = SessionId::create("empty-session").value(),
                           .packagePath = "missing.cstudio",
                           .startedAt = {}},
        [](auto) {});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message(),
              "Start at least one capture source before recording");
}

TEST(FfmpegLiveRecordingEngineTest, ValidatesEveryTrackBeforeAttachingAnySink) {
    auto bindings = std::make_shared<CaptureBindingsFake>();
    auto store = std::make_shared<ProjectPackageStore>();
    const auto duplicate = SourceId::create("duplicate-source").value();
    bindings->sources = {{duplicate, TrackRole::Screen},
                         {duplicate, TrackRole::Microphone}};
    FfmpegLiveRecordingEngine engine{bindings, store};

    const auto result = engine.start(
        LiveRecordingStart{.sessionId = SessionId::create("duplicate-session").value(),
                           .packagePath = "missing.cstudio",
                           .startedAt = {}},
        [](auto) {});

    ASSERT_FALSE(result.hasValue());
    EXPECT_FALSE(bindings->video);
    EXPECT_FALSE(bindings->audio);
    EXPECT_EQ(bindings->detachCalls, 0);
}

}  // namespace
