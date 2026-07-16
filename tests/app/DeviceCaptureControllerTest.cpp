#include "app/DeviceCaptureController.h"

#include "capture/DeviceCaptureTypes.h"
#include "capture/IAudioBlockSink.h"
#include "capture/IDeviceCaptureBackend.h"
#include "capture/IDeviceCaptureSource.h"
#include "capture/IVideoFrameSink.h"
#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "media/MediaTypes.h"

#include <QCoreApplication>
#include <QEvent>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using creator::app::DeviceCaptureController;
using creator::app::DeviceCaptureState;
using creator::capture::CaptureConfig;
using creator::capture::CaptureDeviceInfo;
using creator::capture::CaptureDeviceKind;
using creator::capture::CaptureStats;
using creator::capture::IAudioBlockSink;
using creator::capture::IDeviceCaptureBackend;
using creator::capture::IDeviceCaptureSource;
using creator::capture::IVideoFrameSink;
using creator::capture::MediaPermissionStatus;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::CaptureDeviceId;
using creator::domain::SourceId;
using creator::media::AudioBlock;
using creator::media::PixelFormat;
using creator::media::VideoFrame;

void drainQueuedCalls() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
}

CaptureDeviceInfo device(std::string id, CaptureDeviceKind kind, std::string name,
                         bool isDefault = false) {
    return CaptureDeviceInfo::create(CaptureDeviceId::create(std::move(id)).value(), kind,
                                     std::move(name), isDefault)
        .value();
}

AudioBlock audio(float first, float second) {
    auto samples = std::shared_ptr<float[]>(new float[2], std::default_delete<float[]>{});
    samples[0] = first;
    samples[1] = second;
    AudioBlock block;
    block.timestamp = TimestampNs{ProjectClock::duration{0}};
    block.sampleRate = 48'000;
    block.channels = 1;
    block.frameCount = 2;
    block.samples = std::move(samples);
    return block;
}

VideoFrame video(std::uint32_t width = 1280, std::uint32_t height = 720) {
    return VideoFrame{
        .timestamp = TimestampNs{ProjectClock::duration{0}},
        .width = width,
        .height = height,
        .visibleRect = {0, 0, width, height},
        .contentWidth = width,
        .contentHeight = height,
        .pixelFormat = PixelFormat::Bgra8,
        .platformHandle = std::make_shared<int>(1),
    };
}

struct LifecycleProbe final {
    bool handlerCleared{false};
    int barrierStops{0};
};

class DeviceSourceFake final : public IDeviceCaptureSource {
public:
    DeviceSourceFake(std::shared_ptr<IVideoFrameSink> sink,
                     std::shared_ptr<LifecycleProbe> probe)
        : videoSink(std::move(sink)), probe(std::move(probe)) {}
    DeviceSourceFake(std::shared_ptr<IAudioBlockSink> sink,
                     std::shared_ptr<LifecycleProbe> probe)
        : audioSink(std::move(sink)), probe(std::move(probe)) {}

    [[nodiscard]] SourceId id() const override { return SourceId::create("fake").value(); }
    [[nodiscard]] std::string displayName() const override { return "fake"; }
    [[nodiscard]] Result<void> start(const CaptureConfig&) override {
        ++startCalls;
        if (startError) return *startError;
        if (autoConfirmStart) confirmStarted();
        return creator::core::ok();
    }
    [[nodiscard]] Result<void> stop() override {
        ++barrierStopCalls;
        ++probe->barrierStops;
        stopped = true;
        return creator::core::ok();
    }
    void stopAsync(StopCompletion completion) override {
        ++stopAsyncCalls;
        stopped = true;
        if (deferStop) {
            pendingStop = std::move(completion);
        } else {
            completion(creator::core::ok());
        }
    }
    [[nodiscard]] CaptureStats stats() const noexcept override { return liveStats; }

    void confirmStarted() {
        if (videoSink) videoSink->onCaptureStarted();
        if (audioSink) audioSink->onCaptureStarted();
    }
    void pushVideo(VideoFrame frame) {
        ++liveStats.receivedFrames;
        liveStats.currentFps = 30.0;
        videoSink->onVideoFrame(std::move(frame));
    }
    void pushAudio(AudioBlock block) { audioSink->onAudioBlock(std::move(block)); }
    void fail(AppError error) {
        if (videoSink) videoSink->onCaptureError(std::move(error));
        else audioSink->onCaptureError(std::move(error));
    }
    void completeStop(Result<void> result = creator::core::ok()) {
        auto completion = std::move(*pendingStop);
        pendingStop.reset();
        completion(std::move(result));
    }

    std::shared_ptr<IVideoFrameSink> videoSink;
    std::shared_ptr<IAudioBlockSink> audioSink;
    std::shared_ptr<LifecycleProbe> probe;
    CaptureStats liveStats;
    std::optional<AppError> startError;
    std::optional<StopCompletion> pendingStop;
    bool autoConfirmStart{true};
    bool deferStop{false};
    bool stopped{false};
    int startCalls{0};
    int barrierStopCalls{0};
    int stopAsyncCalls{0};
};

class BackendFake final : public IDeviceCaptureBackend {
public:
    [[nodiscard]] MediaPermissionStatus permissionStatus(
        CaptureDeviceKind kind) const noexcept override {
        return kind == CaptureDeviceKind::Camera ? cameraPermission : microphonePermission;
    }
    void requestPermission(CaptureDeviceKind kind, PermissionCompletion completion) override {
        if (kind == CaptureDeviceKind::Camera) ++cameraPermissionRequests;
        else ++microphonePermissionRequests;
        pendingPermission = std::move(completion);
    }
    [[nodiscard]] Result<std::vector<CaptureDeviceInfo>> devices(
        CaptureDeviceKind kind) override {
        return kind == CaptureDeviceKind::Camera ? cameras : microphones;
    }
    void setDeviceChangeHandler(DeviceChangeHandler handler) override {
        changeHandler = std::move(handler);
        if (!changeHandler) probe->handlerCleared = true;
    }
    [[nodiscard]] Result<std::unique_ptr<IDeviceCaptureSource>> createCamera(
        const CaptureDeviceId&, std::shared_ptr<IVideoFrameSink> sink) override {
        auto source = std::make_unique<DeviceSourceFake>(std::move(sink), probe);
        source->deferStop = deferStops;
        cameraSource = source.get();
        return std::unique_ptr<IDeviceCaptureSource>{std::move(source)};
    }
    [[nodiscard]] Result<std::unique_ptr<IDeviceCaptureSource>> createMicrophone(
        const CaptureDeviceId&, std::shared_ptr<IAudioBlockSink> sink) override {
        auto source = std::make_unique<DeviceSourceFake>(std::move(sink), probe);
        source->deferStop = deferStops;
        microphoneSource = source.get();
        return std::unique_ptr<IDeviceCaptureSource>{std::move(source)};
    }
    [[nodiscard]] Result<std::unique_ptr<IDeviceCaptureSource>> createSystemAudio(
        std::shared_ptr<IAudioBlockSink> sink) override {
        auto source = std::make_unique<DeviceSourceFake>(std::move(sink), probe);
        source->deferStop = deferStops;
        systemAudioSource = source.get();
        return std::unique_ptr<IDeviceCaptureSource>{std::move(source)};
    }

    void completePermission(MediaPermissionStatus status) {
        auto completion = std::move(*pendingPermission);
        pendingPermission.reset();
        completion(status);
    }
    void notifyDevicesChanged() { changeHandler(); }

    MediaPermissionStatus cameraPermission{MediaPermissionStatus::Granted};
    MediaPermissionStatus microphonePermission{MediaPermissionStatus::Granted};
    std::vector<CaptureDeviceInfo> cameras{
        device("camera:external", CaptureDeviceKind::Camera, "External Camera"),
        device("camera:built-in", CaptureDeviceKind::Camera, "Built-in Camera", true)};
    std::vector<CaptureDeviceInfo> microphones{
        device("mic:built-in", CaptureDeviceKind::Microphone, "Built-in Microphone", true)};
    DeviceChangeHandler changeHandler;
    std::optional<PermissionCompletion> pendingPermission;
    DeviceSourceFake* cameraSource{nullptr};
    DeviceSourceFake* microphoneSource{nullptr};
    DeviceSourceFake* systemAudioSource{nullptr};
    bool deferStops{false};
    int cameraPermissionRequests{0};
    int microphonePermissionRequests{0};
    std::shared_ptr<LifecycleProbe> probe{std::make_shared<LifecycleProbe>()};
};

struct Fixture final {
    Fixture() {
        auto backend = std::make_unique<BackendFake>();
        backendRaw = backend.get();
        probe = backend->probe;
        controller = std::make_unique<DeviceCaptureController>(std::move(backend));
    }
    BackendFake* backendRaw{};
    std::shared_ptr<LifecycleProbe> probe;
    std::unique_ptr<DeviceCaptureController> controller;
};

TEST(DeviceCaptureControllerTest, InitializesSnapshotsAndSelectsDefaultDevices) {
    Fixture fixture;

    fixture.controller->initialize();

    EXPECT_EQ(fixture.controller->cameras().size(), 2);
    EXPECT_EQ(fixture.controller->microphones().size(), 1);
    EXPECT_EQ(fixture.controller->selectedCameraId(), QStringLiteral("camera:built-in"));
    EXPECT_EQ(fixture.controller->selectedMicrophoneId(), QStringLiteral("mic:built-in"));
    EXPECT_TRUE(static_cast<bool>(fixture.backendRaw->changeHandler));
}

TEST(DeviceCaptureControllerTest, RequestsCameraAndMicrophonePermissionIndependently) {
    Fixture fixture;
    fixture.backendRaw->cameraPermission = MediaPermissionStatus::Denied;
    fixture.backendRaw->microphonePermission = MediaPermissionStatus::Granted;
    fixture.controller->initialize();
    ASSERT_TRUE(fixture.controller->cameraPermissionRequired());
    ASSERT_FALSE(fixture.controller->microphonePermissionRequired());

    fixture.controller->requestCameraPermission();
    fixture.backendRaw->cameraPermission = MediaPermissionStatus::Granted;
    fixture.backendRaw->completePermission(MediaPermissionStatus::Granted);
    drainQueuedCalls();

    EXPECT_FALSE(fixture.controller->cameraPermissionRequired());
    EXPECT_EQ(fixture.backendRaw->cameraPermissionRequests, 1);
    EXPECT_EQ(fixture.backendRaw->microphonePermissionRequests, 0);
}

TEST(DeviceCaptureControllerTest, StartsCameraMicrophoneAndSystemAudioIndependently) {
    Fixture fixture;
    fixture.controller->initialize();

    fixture.controller->setCameraEnabled(true);
    fixture.controller->setMicrophoneEnabled(true);
    fixture.controller->setSystemAudioEnabled(true);
    fixture.controller->pollCapture();

    EXPECT_TRUE(fixture.controller->cameraCapturing());
    EXPECT_TRUE(fixture.controller->microphoneCapturing());
    EXPECT_TRUE(fixture.controller->systemAudioCapturing());
    ASSERT_NE(fixture.backendRaw->cameraSource, nullptr);
    ASSERT_NE(fixture.backendRaw->microphoneSource, nullptr);
    ASSERT_NE(fixture.backendRaw->systemAudioSource, nullptr);
    EXPECT_EQ(fixture.backendRaw->cameraSource->startCalls, 1);
    EXPECT_EQ(fixture.backendRaw->microphoneSource->startCalls, 1);
    EXPECT_EQ(fixture.backendRaw->systemAudioSource->startCalls, 1);
}

TEST(DeviceCaptureControllerTest, PublishesCameraGeometryAndSeparateAudioLevels) {
    Fixture fixture;
    fixture.controller->initialize();
    fixture.controller->setCameraEnabled(true);
    fixture.controller->setMicrophoneEnabled(true);
    fixture.controller->setSystemAudioEnabled(true);

    fixture.backendRaw->cameraSource->pushVideo(video(1920, 1080));
    fixture.backendRaw->microphoneSource->pushAudio(audio(1.0F, 0.0F));
    fixture.backendRaw->systemAudioSource->pushAudio(audio(0.5F, -0.5F));
    fixture.controller->pollCapture();

    EXPECT_EQ(fixture.controller->cameraWidth(), 1920u);
    EXPECT_EQ(fixture.controller->cameraHeight(), 1080u);
    EXPECT_DOUBLE_EQ(fixture.controller->cameraFps(), 30.0);
    EXPECT_DOUBLE_EQ(fixture.controller->microphonePeakDbfs(), 0.0);
    EXPECT_NEAR(fixture.controller->systemAudioPeakDbfs(), -6.020599913, 1e-6);
    EXPECT_EQ(fixture.controller->microphoneBlocks(), 1u);
    EXPECT_EQ(fixture.controller->systemAudioBlocks(), 1u);
}

TEST(DeviceCaptureControllerTest, RemainsStoppingUntilNativeCompletion) {
    Fixture fixture;
    fixture.backendRaw->deferStops = true;
    fixture.controller->initialize();
    fixture.controller->setCameraEnabled(true);
    fixture.controller->pollCapture();
    auto* source = fixture.backendRaw->cameraSource;

    fixture.controller->setCameraEnabled(false);

    EXPECT_EQ(fixture.controller->cameraState(), DeviceCaptureState::Stopping);
    EXPECT_EQ(source->stopAsyncCalls, 1);
    source->completeStop();
    drainQueuedCalls();
    EXPECT_EQ(fixture.controller->cameraState(), DeviceCaptureState::Ready);
    EXPECT_FALSE(fixture.controller->cameraCapturing());
}

TEST(DeviceCaptureControllerTest, CameraHotUnplugDoesNotStopOtherSourcesOrApp) {
    Fixture fixture;
    fixture.controller->initialize();
    fixture.controller->setCameraEnabled(true);
    fixture.controller->setMicrophoneEnabled(true);
    fixture.controller->setSystemAudioEnabled(true);
    fixture.controller->pollCapture();
    fixture.backendRaw->cameras.clear();

    fixture.backendRaw->notifyDevicesChanged();
    drainQueuedCalls();

    EXPECT_EQ(fixture.controller->cameraState(), DeviceCaptureState::Error);
    EXPECT_TRUE(fixture.controller->cameraStatus().contains("disconnected"));
    EXPECT_TRUE(fixture.controller->microphoneCapturing());
    EXPECT_TRUE(fixture.controller->systemAudioCapturing());
}

TEST(DeviceCaptureControllerTest, MicrophoneErrorIsIsolatedFromCameraAndSystemAudio) {
    Fixture fixture;
    fixture.controller->initialize();
    fixture.controller->setCameraEnabled(true);
    fixture.controller->setMicrophoneEnabled(true);
    fixture.controller->setSystemAudioEnabled(true);
    fixture.controller->pollCapture();

    fixture.backendRaw->microphoneSource->fail(
        {ErrorCode::IoFailure, "microphone device interrupted"});
    fixture.controller->pollCapture();

    EXPECT_EQ(fixture.controller->microphoneState(), DeviceCaptureState::Error);
    EXPECT_EQ(fixture.controller->microphoneStatus(),
              QStringLiteral("microphone device interrupted"));
    EXPECT_TRUE(fixture.controller->cameraCapturing());
    EXPECT_TRUE(fixture.controller->systemAudioCapturing());
}

TEST(DeviceCaptureControllerTest, DestructionClearsHotplugHandlerAndStopsSources) {
    Fixture fixture;
    fixture.controller->initialize();
    fixture.controller->setCameraEnabled(true);
    fixture.controller.reset();

    EXPECT_TRUE(fixture.probe->handlerCleared);
    EXPECT_EQ(fixture.probe->barrierStops, 1);
}

}  // namespace
