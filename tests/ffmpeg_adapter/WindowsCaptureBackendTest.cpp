#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"

#include "capture/AudioCaptureMailbox.h"
#include "capture/LatestVideoFrameMailbox.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"

#include <gtest/gtest.h>

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>

#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <thread>

namespace {

using namespace creator;

TEST(WindowsCaptureBackendTest, DiscoversPhysicalDisplayCameraAndMicrophone) {
    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    ASSERT_TRUE(backend.screenPermission);
    EXPECT_EQ(backend.screenPermission->status(),
              capture::ScreenCapturePermissionStatus::Granted);
    std::promise<core::Result<std::vector<capture::ScreenCaptureTarget>>> promise;
    auto future = promise.get_future();
    backend.screenDiscovery->enumerate(
        [&promise](auto result) { promise.set_value(std::move(result)); });
    ASSERT_EQ(future.wait_for(std::chrono::seconds{5}),
              std::future_status::ready);
    auto displays = future.get();
    ASSERT_TRUE(displays.hasValue()) << displays.error().message();
    ASSERT_FALSE(displays.value().empty());
    EXPECT_GT(displays.value().front().width(), 0U);
    EXPECT_GT(displays.value().front().height(), 0U);

    auto cameras = backend.devices->devices(capture::CaptureDeviceKind::Camera);
    ASSERT_TRUE(cameras.hasValue()) << cameras.error().message();
    EXPECT_FALSE(cameras.value().empty());
    auto microphones =
        backend.devices->devices(capture::CaptureDeviceKind::Microphone);
    ASSERT_TRUE(microphones.hasValue()) << microphones.error().message();
    EXPECT_FALSE(microphones.value().empty());
}

TEST(WindowsCaptureBackendTest, CapturesRealMonotonicBgraDisplayFrames) {
    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    std::promise<core::Result<std::vector<capture::ScreenCaptureTarget>>> promise;
    auto future = promise.get_future();
    backend.screenDiscovery->enumerate(
        [&promise](auto result) { promise.set_value(std::move(result)); });
    auto displays = future.get();
    ASSERT_TRUE(displays.hasValue()) << displays.error().message();
    ASSERT_FALSE(displays.value().empty());
    auto sink = std::make_shared<capture::LatestVideoFrameMailbox>();
    auto source = backend.screenSourceFactory->create(
        displays.value().front().id(), sink);
    ASSERT_TRUE(source.hasValue()) << source.error().message();
    ASSERT_TRUE(source.value()->start({.targetWidth = 1920,
                                      .targetHeight = 1080,
                                      .frameRateNumerator = 30,
                                      .frameRateDenominator = 1})
                    .hasValue());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{10};
    std::optional<media::VideoFrame> first;
    std::optional<media::VideoFrame> second;
    while (std::chrono::steady_clock::now() < deadline && !second.has_value()) {
        if (auto frame = sink->takeLatest()) {
            if (!first.has_value()) {
                first = std::move(frame);
            } else if (frame->timestamp > first->timestamp) {
                second = std::move(frame);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->pixelFormat, media::PixelFormat::Bgra8);
    EXPECT_TRUE(first->platformHandle);
    ffmpeg_adapter::CpuBgraFrameMapper screenMapper;
    EXPECT_TRUE(screenMapper.map(*first).hasValue());
    EXPECT_GT(source.value()->stats().receivedFrames, 1U);
    std::promise<core::Result<void>> stopped;
    auto stoppedFuture = stopped.get_future();
    source.value()->stopAsync(
        [&stopped](auto result) { stopped.set_value(std::move(result)); });
    ASSERT_EQ(stoppedFuture.wait_for(std::chrono::seconds{5}),
              std::future_status::ready);
    EXPECT_TRUE(stoppedFuture.get().hasValue());
}

TEST(WindowsCaptureBackendTest, CapturesPhysicalCameraFrames) {
    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    auto cameras = backend.devices->devices(capture::CaptureDeviceKind::Camera);
    ASSERT_TRUE(cameras.hasValue()) << cameras.error().message();
    ASSERT_FALSE(cameras.value().empty());
    auto sink = std::make_shared<capture::LatestVideoFrameMailbox>();
    auto source = backend.devices->createCamera(cameras.value().front().id(), sink);
    ASSERT_TRUE(source.hasValue()) << source.error().message();
    ASSERT_TRUE(source.value()->start({.targetWidth = 1920,
                                      .targetHeight = 1080,
                                      .frameRateNumerator = 30,
                                      .frameRateDenominator = 1})
                    .hasValue());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{15};
    std::optional<media::VideoFrame> frame;
    while (std::chrono::steady_clock::now() < deadline && !frame.has_value()) {
        frame = sink->takeLatest();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (const auto error = sink->takeError()) FAIL() << error->message();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->pixelFormat, media::PixelFormat::Bgra8);
    EXPECT_TRUE(frame->platformHandle);
    ffmpeg_adapter::CpuBgraFrameMapper cameraMapper;
    EXPECT_TRUE(cameraMapper.map(*frame).hasValue());
    std::promise<core::Result<void>> stopped;
    auto stoppedFuture = stopped.get_future();
    source.value()->stopAsync(
        [&stopped](auto result) { stopped.set_value(std::move(result)); });
    ASSERT_EQ(stoppedFuture.wait_for(std::chrono::seconds{5}),
              std::future_status::ready);
    EXPECT_TRUE(stoppedFuture.get().hasValue());
}

TEST(WindowsCaptureBackendTest, CapturesPhysicalMicrophoneBlocks) {
    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    auto microphones =
        backend.devices->devices(capture::CaptureDeviceKind::Microphone);
    ASSERT_TRUE(microphones.hasValue()) << microphones.error().message();
    ASSERT_FALSE(microphones.value().empty());
    auto sink = std::make_shared<capture::AudioCaptureMailbox>(64);
    auto source =
        backend.devices->createMicrophone(microphones.value().front().id(), sink);
    ASSERT_TRUE(source.hasValue()) << source.error().message();
    ASSERT_TRUE(source.value()->start({}).hasValue());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{15};
    std::optional<media::AudioBlock> block;
    while (std::chrono::steady_clock::now() < deadline && !block.has_value()) {
        block = sink->tryPop();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (const auto error = sink->takeError()) FAIL() << error->message();
    ASSERT_TRUE(block.has_value());
    EXPECT_GT(block->sampleRate, 0U);
    EXPECT_GT(block->channels, 0U);
    EXPECT_GT(block->frameCount, 0U);
    EXPECT_TRUE(block->samples);
    std::promise<core::Result<void>> stopped;
    auto stoppedFuture = stopped.get_future();
    source.value()->stopAsync(
        [&stopped](auto result) { stopped.set_value(std::move(result)); });
    ASSERT_EQ(stoppedFuture.wait_for(std::chrono::seconds{5}),
              std::future_status::ready);
    EXPECT_TRUE(stoppedFuture.get().hasValue());
}

TEST(WindowsCaptureBackendTest, CapturesDefaultSpeakerThroughWasapiLoopback) {
    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    auto sink = std::make_shared<capture::AudioCaptureMailbox>(128);
    auto source = backend.devices->createSystemAudio(sink);
    ASSERT_TRUE(source.hasValue()) << source.error().message();
    ASSERT_TRUE(source.value()->start({}).hasValue());
    const auto mediaPath =
        std::wstring{L"C:\\Windows\\Media\\Windows Notify System Generic.wav"};
    ASSERT_NE(PlaySoundW(mediaPath.c_str(), nullptr,
                         SND_FILENAME | SND_ASYNC | SND_NODEFAULT),
              FALSE);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{15};
    std::optional<media::AudioBlock> block;
    bool heardSignal = false;
    while (std::chrono::steady_clock::now() < deadline && !heardSignal) {
        if (auto next = sink->tryPop()) {
            block = next;
            const auto count = static_cast<std::size_t>(next->frameCount) *
                               next->channels;
            for (std::size_t index = 0; index < count; ++index) {
                if (std::abs(next->samples[index]) > 0.0001F) {
                    heardSignal = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    PlaySoundW(nullptr, nullptr, 0);
    if (const auto error = sink->takeError()) FAIL() << error->message();
    ASSERT_TRUE(block.has_value());
    EXPECT_TRUE(heardSignal);
    EXPECT_GT(source.value()->stats().receivedFrames, 0U);
    std::promise<core::Result<void>> stopped;
    auto stoppedFuture = stopped.get_future();
    source.value()->stopAsync(
        [&stopped](auto result) { stopped.set_value(std::move(result)); });
    ASSERT_EQ(stoppedFuture.wait_for(std::chrono::seconds{5}),
              std::future_status::ready);
    EXPECT_TRUE(stoppedFuture.get().hasValue());
}

}  // namespace

#endif
