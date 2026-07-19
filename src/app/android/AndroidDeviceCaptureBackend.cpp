#include "app/android/AndroidDeviceCaptureBackend.h"

#include "capture/AudioCaptureBlockAssembler.h"
#include "capture/CameraCaptureFrameAssembler.h"
#include "core/AppError.h"

#include <QJniEnvironment>
#include <QJniObject>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace creator::app::android {
namespace {

constexpr const char* kActivityClass =
    "com/studioonair/creatorstudio/CreatorStudioActivity";
constexpr int kCamera = 0;
constexpr int kMicrophone = 1;

capture::MediaPermissionStatus permissionFromJava(jint status) noexcept {
    switch (status) {
    case 1: return capture::MediaPermissionStatus::Granted;
    case 2: return capture::MediaPermissionStatus::Denied;
    case 3: return capture::MediaPermissionStatus::Restricted;
    default: return capture::MediaPermissionStatus::Unknown;
    }
}

int javaKind(capture::CaptureDeviceKind kind) noexcept {
    return kind == capture::CaptureDeviceKind::Camera ? kCamera : kMicrophone;
}

class AndroidDeviceBackend;
class AndroidCameraSource;
class AndroidMicrophoneSource;
std::mutex callbackMutex;
AndroidDeviceBackend* callbackBackend{};
AndroidCameraSource* callbackCamera{};
AndroidMicrophoneSource* callbackMicrophone{};
AndroidMicrophoneSource* callbackSystemAudio{};

class AndroidCameraSource final : public capture::IDeviceCaptureSource {
public:
    explicit AndroidCameraSource(std::shared_ptr<capture::IVideoFrameSink> sink)
        : sink_(std::move(sink)), id_(domain::SourceId::create("android-camera").value()) {}
    ~AndroidCameraSource() override { static_cast<void>(stop()); }

    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return "Android camera"; }
    [[nodiscard]] core::Result<void> start(const capture::CaptureConfig& config) override {
        if (!sink_) return core::AppError{core::ErrorCode::InvalidArgument, "camera sink is required"};
        if (started_.exchange(true)) return core::AppError{core::ErrorCode::InvalidState, "camera is already started"};
        generation_ = nextGeneration_.fetch_add(1);
        {
            std::lock_guard lock(callbackMutex);
            callbackCamera = this;
        }
        QJniObject::callStaticMethod<void>(kActivityClass, "startCamera", "(JIII)V",
                                            static_cast<jlong>(generation_),
                                            static_cast<jint>(config.targetWidth),
                                            static_cast<jint>(config.targetHeight),
                                            static_cast<jint>(config.frameRateNumerator));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            clearCallback();
            started_ = false;
            return core::AppError{core::ErrorCode::IoFailure, "Android could not start Camera2"};
        }
        return core::ok();
    }
    [[nodiscard]] core::Result<void> stop() override {
        if (!started_.exchange(false)) return core::ok();
        clearCallback();
        QJniObject::callStaticMethod<void>(kActivityClass, "stopCamera", "(J)V",
                                            static_cast<jlong>(generation_));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            return core::AppError{core::ErrorCode::IoFailure, "Android could not stop Camera2"};
        }
        return core::ok();
    }
    void stopAsync(StopCompletion completion) override {
        const auto result = stop();
        if (completion) completion(result);
    }
    [[nodiscard]] capture::CaptureStats stats() const noexcept override {
        std::lock_guard lock(mutex_); return stats_;
    }
    void frame(std::uint64_t generation, const std::byte* rgba, std::size_t bytes,
               std::uint32_t width, std::uint32_t height, std::int64_t timestampNs) noexcept {
        if (generation != generation_ || !started_ || !rgba || width == 0 || height == 0 ||
            bytes < static_cast<std::size_t>(width) * height * 4) return;
        try {
            auto pixels = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(width) * height * 4);
            std::memcpy(pixels->data(), rgba, pixels->size());
            auto assembled = assembler_.assemble(capture::NativeCameraFrame{
                .timestamp = {timestampNs, 1'000'000'000}, .width = width, .height = height,
                .pixelFormat = media::PixelFormat::Bgra8, .platformHandle = std::move(pixels)});
            if (!assembled.hasValue()) { invalid(); return; }
            if (!announced_.exchange(true)) sink_->onCaptureStarted();
            sink_->onVideoFrame(std::move(assembled).value());
            std::lock_guard lock(mutex_); ++stats_.receivedFrames;
        } catch (...) { failed("Android camera frame import failed"); }
    }
    void failed(const char* message) noexcept {
        if (started_.exchange(false) && sink_) {
            sink_->onCaptureError({core::ErrorCode::IoFailure, message});
        }
    }
private:
    void clearCallback() noexcept {
        std::lock_guard lock(callbackMutex);
        if (callbackCamera == this) callbackCamera = nullptr;
    }
    void invalid() noexcept { std::lock_guard lock(mutex_); ++stats_.invalidFrames; }
    std::shared_ptr<capture::IVideoFrameSink> sink_;
    domain::SourceId id_;
    capture::CameraCaptureFrameAssembler assembler_;
    mutable std::mutex mutex_;
    capture::CaptureStats stats_{};
    std::atomic_bool started_{};
    std::atomic_bool announced_{};
    std::uint64_t generation_{};
    static inline std::atomic_uint64_t nextGeneration_{1};
};

class AndroidMicrophoneSource final : public capture::IDeviceCaptureSource {
public:
    AndroidMicrophoneSource(std::shared_ptr<capture::IAudioBlockSink> sink, bool systemAudio)
        : sink_(std::move(sink)), systemAudio_(systemAudio),
          id_(domain::SourceId::create(systemAudio ? "android-system-audio" : "android-microphone").value()) {}
    ~AndroidMicrophoneSource() override { static_cast<void>(stop()); }
    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override {
        return systemAudio_ ? "Android playback audio" : "Android microphone";
    }
    [[nodiscard]] core::Result<void> start(const capture::CaptureConfig&) override {
        if (!sink_) return core::AppError{core::ErrorCode::InvalidArgument, "microphone sink is required"};
        if (started_.exchange(true)) return core::AppError{core::ErrorCode::InvalidState, "microphone is already started"};
        generation_ = nextGeneration_.fetch_add(1);
        { std::lock_guard lock(callbackMutex);
          if (systemAudio_) callbackSystemAudio = this; else callbackMicrophone = this; }
        QJniObject::callStaticMethod<void>(kActivityClass,
                                            systemAudio_ ? "startPlaybackAudio" : "startMicrophone", "(J)V",
                                            static_cast<jlong>(generation_));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            clearCallback(); started_ = false;
            return core::AppError{core::ErrorCode::IoFailure,
                                  systemAudio_ ? "Android could not start playback capture" : "Android could not start AudioRecord"};
        }
        return core::ok();
    }
    [[nodiscard]] core::Result<void> stop() override {
        if (!started_.exchange(false)) return core::ok();
        clearCallback();
        QJniObject::callStaticMethod<void>(kActivityClass,
                                            systemAudio_ ? "stopPlaybackAudio" : "stopMicrophone", "(J)V",
                                            static_cast<jlong>(generation_));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  systemAudio_ ? "Android could not stop playback capture" : "Android could not stop AudioRecord"};
        }
        return core::ok();
    }
    void stopAsync(StopCompletion completion) override { const auto result = stop(); if (completion) completion(result); }
    [[nodiscard]] capture::CaptureStats stats() const noexcept override { return {}; }
    void pcm16(std::uint64_t generation, const std::int16_t* pcm, std::size_t samples,
               std::uint32_t sampleRate, std::uint32_t channels, std::int64_t timestampNs) noexcept {
        if (generation != generation_ || !started_ || !pcm || samples == 0 || channels == 0 ||
            samples % channels != 0) return;
        try {
            auto converted = std::shared_ptr<float[]>{new float[samples]};
            for (std::size_t i = 0; i < samples; ++i) converted[i] = static_cast<float>(pcm[i]) / 32768.0F;
            auto assembled = assembler_.assemble(capture::NativeAudioBlock{
                .timestamp = {timestampNs, 1'000'000'000}, .sampleRate = sampleRate,
                .channels = channels, .frameCount = static_cast<std::uint32_t>(samples / channels),
                .sampleCount = samples, .samples = std::move(converted)});
            if (!assembled.hasValue()) { failed("Android microphone block import failed"); return; }
            if (!announced_.exchange(true)) sink_->onCaptureStarted();
            sink_->onAudioBlock(std::move(assembled).value());
        } catch (...) { failed("Android microphone callback failed"); }
    }
    void failed(const char* message) noexcept {
        if (started_.exchange(false) && sink_) sink_->onCaptureError({core::ErrorCode::IoFailure, message});
    }
private:
    void clearCallback() noexcept {
        std::lock_guard lock(callbackMutex);
        auto& callback = systemAudio_ ? callbackSystemAudio : callbackMicrophone;
        if (callback == this) callback = nullptr;
    }
    std::shared_ptr<capture::IAudioBlockSink> sink_;
    bool systemAudio_{};
    domain::SourceId id_;
    capture::AudioCaptureBlockAssembler assembler_;
    std::atomic_bool started_{};
    std::atomic_bool announced_{};
    std::uint64_t generation_{};
    static inline std::atomic_uint64_t nextGeneration_{1};
};

class AndroidDeviceBackend final : public capture::IDeviceCaptureBackend {
public:
    AndroidDeviceBackend() { std::lock_guard lock(callbackMutex); callbackBackend = this; }
    ~AndroidDeviceBackend() override { std::lock_guard lock(callbackMutex); if (callbackBackend == this) callbackBackend = nullptr; }
    [[nodiscard]] capture::MediaPermissionStatus permissionStatus(capture::CaptureDeviceKind kind) const noexcept override {
        const auto result = QJniObject::callStaticMethod<jint>(kActivityClass, "mediaPermissionStatus", "(I)I", javaKind(kind));
        QJniEnvironment environment;
        return environment.checkAndClearExceptions() ? capture::MediaPermissionStatus::Unknown : permissionFromJava(result);
    }
    void requestPermission(capture::CaptureDeviceKind kind, PermissionCompletion completion) override {
        { std::lock_guard lock(mutex_); permissions_[javaKind(kind)] = std::move(completion); }
        const auto generation = ++permissionGeneration_[javaKind(kind)];
        QJniObject::callStaticMethod<void>(kActivityClass, "requestMediaPermission", "(IJ)V", javaKind(kind), static_cast<jlong>(generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) resolvePermission(javaKind(kind), generation, false);
    }
    [[nodiscard]] core::Result<std::vector<capture::CaptureDeviceInfo>> devices(capture::CaptureDeviceKind kind) override {
        const auto idText = kind == capture::CaptureDeviceKind::Camera ? "android-camera-default" : "android-microphone-default";
        auto id = domain::CaptureDeviceId::create(idText);
        if (!id.hasValue()) return id.error();
        auto device = capture::CaptureDeviceInfo::create(std::move(id).value(), kind,
            kind == capture::CaptureDeviceKind::Camera ? "Android default camera" : "Android default microphone", true);
        if (!device.hasValue()) return device.error();
        return std::vector<capture::CaptureDeviceInfo>{std::move(device).value()};
    }
    void setDeviceChangeHandler(DeviceChangeHandler handler) override { std::lock_guard lock(mutex_); deviceHandler_ = std::move(handler); }
    [[nodiscard]] core::Result<std::unique_ptr<capture::IDeviceCaptureSource>> createCamera(
        const domain::CaptureDeviceId&, std::shared_ptr<capture::IVideoFrameSink> sink) override {
        if (!sink) return core::AppError{core::ErrorCode::InvalidArgument, "camera sink is required"};
        return std::unique_ptr<capture::IDeviceCaptureSource>{std::make_unique<AndroidCameraSource>(std::move(sink))};
    }
    [[nodiscard]] core::Result<std::unique_ptr<capture::IDeviceCaptureSource>> createMicrophone(
        const domain::CaptureDeviceId&, std::shared_ptr<capture::IAudioBlockSink> sink) override {
        if (!sink) return core::AppError{core::ErrorCode::InvalidArgument, "microphone sink is required"};
        return std::unique_ptr<capture::IDeviceCaptureSource>{std::make_unique<AndroidMicrophoneSource>(std::move(sink), false)};
    }
    [[nodiscard]] core::Result<std::unique_ptr<capture::IDeviceCaptureSource>> createSystemAudio(
        std::shared_ptr<capture::IAudioBlockSink> sink) override {
        if (!sink) return core::AppError{core::ErrorCode::InvalidArgument, "system-audio sink is required"};
        return std::unique_ptr<capture::IDeviceCaptureSource>{std::make_unique<AndroidMicrophoneSource>(std::move(sink), true)};
    }
    void resolvePermission(int kind, std::uint64_t generation, bool granted) {
        PermissionCompletion completion;
        { std::lock_guard lock(mutex_);
          if (permissionGeneration_[kind] != generation) return;
          completion = std::move(permissions_[kind]); }
        if (completion) completion(granted ? core::Result<capture::MediaPermissionStatus>{capture::MediaPermissionStatus::Granted}
                                          : core::Result<capture::MediaPermissionStatus>{capture::MediaPermissionStatus::Denied});
    }
private:
    mutable std::mutex mutex_;
    PermissionCompletion permissions_[2];
    std::uint64_t permissionGeneration_[2]{};
    DeviceChangeHandler deviceHandler_;
};

}  // namespace

std::unique_ptr<capture::IDeviceCaptureBackend> makeAndroidDeviceCaptureBackend() {
    return std::make_unique<AndroidDeviceBackend>();
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMediaPermissionResult(
    JNIEnv*, jclass, jint kind, jlong generation, jboolean granted) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (creator::app::android::callbackBackend) creator::app::android::callbackBackend->resolvePermission(
        kind, static_cast<std::uint64_t>(generation), granted == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeCameraFrame(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint width, jint height, jlong timestampNs) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (!creator::app::android::callbackCamera) return;
    auto* bytes = static_cast<std::byte*>(environment->GetDirectBufferAddress(buffer));
    const auto size = environment->GetDirectBufferCapacity(buffer);
    creator::app::android::callbackCamera->frame(static_cast<std::uint64_t>(generation), bytes,
        size < 0 ? 0 : static_cast<std::size_t>(size), width, height, timestampNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMicrophonePcm16(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint sampleCount,
    jint sampleRate, jint channels, jlong timestampNs) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (!creator::app::android::callbackMicrophone || sampleCount <= 0) return;
    auto* pcm = static_cast<std::int16_t*>(environment->GetDirectBufferAddress(buffer));
    creator::app::android::callbackMicrophone->pcm16(static_cast<std::uint64_t>(generation), pcm,
        static_cast<std::size_t>(sampleCount), sampleRate, channels, timestampNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeCameraFailed(JNIEnv*, jclass, jlong) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (creator::app::android::callbackCamera) creator::app::android::callbackCamera->failed("Android Camera2 failed");
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMicrophoneFailed(JNIEnv*, jclass, jlong) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (creator::app::android::callbackMicrophone) creator::app::android::callbackMicrophone->failed("Android AudioRecord failed");
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeSystemAudioPcm16(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint sampleCount,
    jint sampleRate, jint channels, jlong timestampNs) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (!creator::app::android::callbackSystemAudio || sampleCount <= 0) return;
    auto* pcm = static_cast<std::int16_t*>(environment->GetDirectBufferAddress(buffer));
    creator::app::android::callbackSystemAudio->pcm16(static_cast<std::uint64_t>(generation), pcm,
        static_cast<std::size_t>(sampleCount), sampleRate, channels, timestampNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeSystemAudioFailed(JNIEnv*, jclass, jlong) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (creator::app::android::callbackSystemAudio) {
        creator::app::android::callbackSystemAudio->failed("Android playback audio capture failed");
    }
}

}  // namespace creator::app::android
