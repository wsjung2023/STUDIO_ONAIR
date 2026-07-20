#include "app/android/AndroidDeviceCaptureBackend.h"

#include "capture/AudioCaptureBlockAssembler.h"
#include "capture/AndroidDeviceSession.h"
#include "capture/CameraCaptureFrameAssembler.h"
#include "core/AppError.h"

#include <QJniEnvironment>
#include <QJniObject>

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

class AndroidPermissionState;
class AndroidCameraState;
class AndroidAudioState;
std::mutex callbackMutex;
std::weak_ptr<AndroidPermissionState> callbackBackend;
std::shared_ptr<AndroidCameraState> callbackCamera;
std::shared_ptr<AndroidAudioState> callbackMicrophone;
std::shared_ptr<AndroidAudioState> callbackSystemAudio;

class AndroidCameraState final {
public:
    explicit AndroidCameraState(std::shared_ptr<capture::IVideoFrameSink> sink)
        : sink_(std::move(sink)) {}

    [[nodiscard]] std::optional<std::uint64_t> begin() {
        std::lock_guard lock(mutex_);
        if (session_.state() == capture::DeviceSessionState::Starting ||
            session_.state() == capture::DeviceSessionState::Streaming ||
            session_.state() == capture::DeviceSessionState::Stopping ||
            session_.state() == capture::DeviceSessionState::Failed) {
            return std::nullopt;
        }
        announced_ = false;
        terminalError_.reset();
        stopCompletions_.clear();
        stats_ = {};
        return session_.begin();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        std::lock_guard lock(mutex_);
        return session_.generation();
    }

    [[nodiscard]] bool requestStop(capture::IDeviceCaptureSource::StopCompletion completion) {
        capture::IDeviceCaptureSource::StopCompletion immediate;
        bool requestJavaStop = false;
        {
            std::lock_guard lock(mutex_);
            if (session_.state() == capture::DeviceSessionState::Stopped ||
                session_.state() == capture::DeviceSessionState::Idle) {
                immediate = std::move(completion);
            } else {
                if (completion) stopCompletions_.push_back(std::move(completion));
                if (session_.state() != capture::DeviceSessionState::Stopping) {
                    requestJavaStop = session_.requestStop(session_.generation());
                }
            }
        }
        if (immediate) immediate(core::ok());
        return requestJavaStop;
    }

    [[nodiscard]] capture::CaptureStats stats() const noexcept {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    void frame(std::uint64_t generation, const std::byte* rgba, std::size_t bytes,
               std::uint32_t width, std::uint32_t height, std::int64_t timestampNs) noexcept {
        std::lock_guard lock(mutex_);
        if (!session_.acceptsCallbacks(generation)) {
            ++stats_.ignoredFrames;
            return;
        }
        if (!rgba || width == 0 || height == 0 ||
            bytes < static_cast<std::size_t>(width) * height * 4) {
            ++stats_.invalidFrames;
            return;
        }
        try {
            auto pixels = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(width) * height * 4);
            std::memcpy(pixels->data(), rgba, pixels->size());
            auto assembled = assembler_.assemble(capture::NativeCameraFrame{
                .timestamp = {timestampNs, 1'000'000'000}, .width = width, .height = height,
                .pixelFormat = media::PixelFormat::Bgra8, .platformHandle = std::move(pixels)});
            if (!assembled.hasValue()) { ++stats_.invalidFrames; return; }
            if (session_.state() == capture::DeviceSessionState::Starting) {
                static_cast<void>(session_.markStreaming(generation));
            }
            if (!announced_) { announced_ = true; sink_->onCaptureStarted(); }
            sink_->onVideoFrame(std::move(assembled).value());
            ++stats_.receivedFrames;
        } catch (...) {
            failLocked(generation, "Android camera frame import failed");
        }
    }

    void failed(std::uint64_t generation, const char* message) noexcept {
        std::lock_guard lock(mutex_);
        failLocked(generation, message);
    }

    void stopped(std::uint64_t generation) {
        std::vector<capture::IDeviceCaptureSource::StopCompletion> completions;
        std::optional<core::AppError> error;
        {
            std::lock_guard lock(mutex_);
            if (session_.state() == capture::DeviceSessionState::Failed) {
                static_cast<void>(session_.requestStop(generation));
            }
            if (!session_.markStopped(generation)) return;
            completions = std::move(stopCompletions_);
            error = terminalError_;
        }
        for (auto& completion : completions) {
            if (!completion) continue;
            if (error) completion(*error);
            else completion(core::ok());
        }
    }

    void failStop(core::AppError error) {
        std::vector<capture::IDeviceCaptureSource::StopCompletion> completions;
        {
            std::lock_guard lock(mutex_);
            static_cast<void>(session_.markStopped(session_.generation()));
            completions = std::move(stopCompletions_);
        }
        for (auto& completion : completions) if (completion) completion(error);
    }

private:
    void failLocked(std::uint64_t generation, const char* message) noexcept {
        if (!session_.fail(generation) || !sink_) return;
        terminalError_ = core::AppError{core::ErrorCode::IoFailure, message};
        sink_->onCaptureError(*terminalError_);
    }

    std::shared_ptr<capture::IVideoFrameSink> sink_;
    capture::CameraCaptureFrameAssembler assembler_;
    mutable std::mutex mutex_;
    capture::AndroidDeviceSession session_;
    capture::CaptureStats stats_{};
    bool announced_{};
    std::optional<core::AppError> terminalError_;
    std::vector<capture::IDeviceCaptureSource::StopCompletion> stopCompletions_;
};

class AndroidCameraSource final : public capture::IDeviceCaptureSource {
public:
    explicit AndroidCameraSource(std::shared_ptr<capture::IVideoFrameSink> sink)
        : state_(std::make_shared<AndroidCameraState>(std::move(sink))),
          id_(domain::SourceId::create("android-camera").value()) {}
    ~AndroidCameraSource() override { static_cast<void>(stop()); }

    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return "Android camera"; }
    [[nodiscard]] core::Result<void> start(const capture::CaptureConfig& config) override {
        const auto generation = state_->begin();
        if (!generation) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Android camera is already active"};
        }
        {
            std::lock_guard lock(callbackMutex);
            callbackCamera = state_;
        }
        QJniObject::callStaticMethod<void>(kActivityClass, "startCamera", "(JIII)V",
                                            static_cast<jlong>(*generation),
                                            static_cast<jint>(config.targetWidth),
                                            static_cast<jint>(config.targetHeight),
                                            static_cast<jint>(config.frameRateNumerator));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            auto error = core::AppError{core::ErrorCode::IoFailure,
                                        "Android could not start Camera2"};
            static_cast<void>(state_->requestStop({}));
            state_->failStop(error);
            clearCallback();
            return error;
        }
        return core::ok();
    }
    [[nodiscard]] core::Result<void> stop() override { return requestStop({}); }
    void stopAsync(StopCompletion completion) override {
        static_cast<void>(requestStop(std::move(completion)));
    }
    [[nodiscard]] capture::CaptureStats stats() const noexcept override { return state_->stats(); }

private:
    [[nodiscard]] core::Result<void> requestStop(StopCompletion completion) {
        const auto generation = state_->generation();
        if (!state_->requestStop(std::move(completion))) return core::ok();
        QJniObject::callStaticMethod<void>(kActivityClass, "stopCamera", "(J)V",
                                            static_cast<jlong>(generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            auto error = core::AppError{core::ErrorCode::IoFailure,
                                        "Android could not stop Camera2"};
            state_->failStop(error);
            clearCallback();
            return error;
        }
        return core::ok();
    }
    void clearCallback() noexcept {
        std::lock_guard lock(callbackMutex);
        if (callbackCamera == state_) callbackCamera.reset();
    }

    std::shared_ptr<AndroidCameraState> state_;
    domain::SourceId id_;
};

class AndroidAudioState final {
public:
    AndroidAudioState(std::shared_ptr<capture::IAudioBlockSink> sink, bool systemAudio)
        : sink_(std::move(sink)), systemAudio_(systemAudio) {}

    [[nodiscard]] std::optional<std::uint64_t> begin() {
        std::lock_guard lock(mutex_);
        if (session_.state() == capture::DeviceSessionState::Starting ||
            session_.state() == capture::DeviceSessionState::Streaming ||
            session_.state() == capture::DeviceSessionState::Stopping ||
            session_.state() == capture::DeviceSessionState::Failed) {
            return std::nullopt;
        }
        announced_ = false;
        terminalError_.reset();
        stopCompletions_.clear();
        stats_ = {};
        return session_.begin();
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        std::lock_guard lock(mutex_);
        return session_.generation();
    }
    [[nodiscard]] bool requestStop(capture::IDeviceCaptureSource::StopCompletion completion) {
        capture::IDeviceCaptureSource::StopCompletion immediate;
        bool requestJavaStop = false;
        {
            std::lock_guard lock(mutex_);
            if (session_.state() == capture::DeviceSessionState::Stopped ||
                session_.state() == capture::DeviceSessionState::Idle) {
                immediate = std::move(completion);
            } else {
                if (completion) stopCompletions_.push_back(std::move(completion));
                if (session_.state() != capture::DeviceSessionState::Stopping) {
                    requestJavaStop = session_.requestStop(session_.generation());
                }
            }
        }
        if (immediate) immediate(core::ok());
        return requestJavaStop;
    }
    [[nodiscard]] capture::CaptureStats stats() const noexcept {
        std::lock_guard lock(mutex_);
        return stats_;
    }
    void pcm16(std::uint64_t generation, const std::int16_t* pcm, std::size_t samples,
               std::uint32_t sampleRate, std::uint32_t channels,
               std::int64_t timestampNs) noexcept {
        std::lock_guard lock(mutex_);
        if (!session_.acceptsCallbacks(generation)) {
            ++stats_.ignoredFrames;
            return;
        }
        if (!pcm || samples == 0 || channels == 0 || samples % channels != 0) {
            ++stats_.invalidFrames;
            return;
        }
        try {
            auto converted = std::shared_ptr<float[]>{new float[samples]};
            for (std::size_t i = 0; i < samples; ++i) {
                converted[i] = static_cast<float>(pcm[i]) / 32768.0F;
            }
            auto assembled = assembler_.assemble(capture::NativeAudioBlock{
                .timestamp = {timestampNs, 1'000'000'000}, .sampleRate = sampleRate,
                .channels = channels,
                .frameCount = static_cast<std::uint32_t>(samples / channels),
                .sampleCount = samples, .samples = std::move(converted)});
            if (!assembled.hasValue()) {
                ++stats_.invalidFrames;
                return;
            }
            if (session_.state() == capture::DeviceSessionState::Starting) {
                static_cast<void>(session_.markStreaming(generation));
            }
            if (!announced_) { announced_ = true; sink_->onCaptureStarted(); }
            sink_->onAudioBlock(std::move(assembled).value());
            ++stats_.receivedFrames;
        } catch (...) {
            failLocked(generation, systemAudio_ ? "Android playback audio callback failed"
                                                : "Android microphone callback failed");
        }
    }
    void failed(std::uint64_t generation, const char* message) noexcept {
        std::lock_guard lock(mutex_);
        failLocked(generation, message);
    }
    void stopped(std::uint64_t generation) {
        std::vector<capture::IDeviceCaptureSource::StopCompletion> completions;
        std::optional<core::AppError> error;
        {
            std::lock_guard lock(mutex_);
            if (session_.state() == capture::DeviceSessionState::Failed) {
                static_cast<void>(session_.requestStop(generation));
            }
            if (!session_.markStopped(generation)) return;
            completions = std::move(stopCompletions_);
            error = terminalError_;
        }
        for (auto& completion : completions) {
            if (!completion) continue;
            if (error) completion(*error);
            else completion(core::ok());
        }
    }
    void failStop(core::AppError error) {
        std::vector<capture::IDeviceCaptureSource::StopCompletion> completions;
        {
            std::lock_guard lock(mutex_);
            static_cast<void>(session_.markStopped(session_.generation()));
            completions = std::move(stopCompletions_);
        }
        for (auto& completion : completions) if (completion) completion(error);
    }

private:
    void failLocked(std::uint64_t generation, const char* message) noexcept {
        if (!session_.fail(generation) || !sink_) return;
        terminalError_ = core::AppError{core::ErrorCode::IoFailure, message};
        sink_->onCaptureError(*terminalError_);
    }

    std::shared_ptr<capture::IAudioBlockSink> sink_;
    bool systemAudio_{};
    capture::AudioCaptureBlockAssembler assembler_;
    mutable std::mutex mutex_;
    capture::AndroidDeviceSession session_;
    capture::CaptureStats stats_{};
    bool announced_{};
    std::optional<core::AppError> terminalError_;
    std::vector<capture::IDeviceCaptureSource::StopCompletion> stopCompletions_;
};

class AndroidMicrophoneSource final : public capture::IDeviceCaptureSource {
public:
    AndroidMicrophoneSource(std::shared_ptr<capture::IAudioBlockSink> sink, bool systemAudio)
        : state_(std::make_shared<AndroidAudioState>(std::move(sink), systemAudio)),
          systemAudio_(systemAudio),
          id_(domain::SourceId::create(systemAudio ? "android-system-audio" : "android-microphone").value()) {}
    ~AndroidMicrophoneSource() override { static_cast<void>(stop()); }
    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override {
        return systemAudio_ ? "Android playback audio" : "Android microphone";
    }
    [[nodiscard]] core::Result<void> start(const capture::CaptureConfig&) override {
        const auto generation = state_->begin();
        if (!generation) {
            return core::AppError{
                core::ErrorCode::InvalidState,
                systemAudio_ ? "Android playback audio is already active"
                             : "Android microphone is already active"};
        }
        {
            std::lock_guard lock(callbackMutex);
            auto& callback = systemAudio_ ? callbackSystemAudio : callbackMicrophone;
            callback = state_;
        }
        QJniObject::callStaticMethod<void>(kActivityClass,
                                            systemAudio_ ? "startPlaybackAudio" : "startMicrophone",
                                            "(J)V", static_cast<jlong>(*generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            auto error = core::AppError{
                core::ErrorCode::IoFailure,
                systemAudio_ ? "Android could not start playback capture"
                             : "Android could not start AudioRecord"};
            static_cast<void>(state_->requestStop({}));
            state_->failStop(error);
            clearCallback();
            return error;
        }
        return core::ok();
    }
    [[nodiscard]] core::Result<void> stop() override { return requestStop({}); }
    void stopAsync(StopCompletion completion) override {
        static_cast<void>(requestStop(std::move(completion)));
    }
    [[nodiscard]] capture::CaptureStats stats() const noexcept override { return state_->stats(); }

private:
    [[nodiscard]] core::Result<void> requestStop(StopCompletion completion) {
        const auto generation = state_->generation();
        if (!state_->requestStop(std::move(completion))) return core::ok();
        QJniObject::callStaticMethod<void>(kActivityClass,
                                            systemAudio_ ? "stopPlaybackAudio" : "stopMicrophone",
                                            "(J)V", static_cast<jlong>(generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            auto error = core::AppError{
                core::ErrorCode::IoFailure,
                systemAudio_ ? "Android could not stop playback capture"
                             : "Android could not stop AudioRecord"};
            state_->failStop(error);
            clearCallback();
            return error;
        }
        return core::ok();
    }
    void clearCallback() noexcept {
        std::lock_guard lock(callbackMutex);
        auto& callback = systemAudio_ ? callbackSystemAudio : callbackMicrophone;
        if (callback == state_) callback.reset();
    }

    std::shared_ptr<AndroidAudioState> state_;
    bool systemAudio_{};
    domain::SourceId id_;
};

class AndroidPermissionState final {
public:
    using PermissionCompletion = capture::IDeviceCaptureBackend::PermissionCompletion;

    [[nodiscard]] std::optional<std::uint64_t> begin(
        int kind, PermissionCompletion& completion) {
        std::lock_guard lock(mutex_);
        if (kind < kCamera || kind > kMicrophone || permissions_[kind]) return std::nullopt;
        permissions_[kind] = std::move(completion);
        return ++permissionGeneration_[kind];
    }

    void resolve(int kind, std::uint64_t generation, bool granted) {
        PermissionCompletion completion;
        {
            std::lock_guard lock(mutex_);
            if (kind < kCamera || kind > kMicrophone ||
                permissionGeneration_[kind] != generation) {
                return;
            }
            completion = std::move(permissions_[kind]);
        }
        if (completion) {
            completion(granted ? core::Result<capture::MediaPermissionStatus>{
                                     capture::MediaPermissionStatus::Granted}
                               : core::Result<capture::MediaPermissionStatus>{
                                     capture::MediaPermissionStatus::Denied});
        }
    }

private:
    std::mutex mutex_;
    PermissionCompletion permissions_[2];
    std::uint64_t permissionGeneration_[2]{};
};

class AndroidDeviceBackend final : public capture::IDeviceCaptureBackend {
public:
    AndroidDeviceBackend() : permissions_(std::make_shared<AndroidPermissionState>()) {
        std::lock_guard lock(callbackMutex);
        callbackBackend = permissions_;
    }
    ~AndroidDeviceBackend() override {
        std::lock_guard lock(callbackMutex);
        if (callbackBackend.lock() == permissions_) callbackBackend.reset();
    }
    [[nodiscard]] capture::MediaPermissionStatus permissionStatus(capture::CaptureDeviceKind kind) const noexcept override {
        const auto result = QJniObject::callStaticMethod<jint>(kActivityClass, "mediaPermissionStatus", "(I)I", javaKind(kind));
        QJniEnvironment environment;
        return environment.checkAndClearExceptions() ? capture::MediaPermissionStatus::Unknown : permissionFromJava(result);
    }
    void requestPermission(capture::CaptureDeviceKind kind, PermissionCompletion completion) override {
        const auto requestedKind = javaKind(kind);
        const auto generation = permissions_->begin(requestedKind, completion);
        if (!generation) {
            if (completion) completion(core::AppError{
                core::ErrorCode::InvalidState,
                "Android media permission request is already pending"});
            return;
        }
        QJniObject::callStaticMethod<void>(kActivityClass, "requestMediaPermission", "(IJ)V",
                                            requestedKind, static_cast<jlong>(*generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            permissions_->resolve(requestedKind, *generation, false);
        }
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
private:
    mutable std::mutex mutex_;
    std::shared_ptr<AndroidPermissionState> permissions_;
    DeviceChangeHandler deviceHandler_;
};

std::shared_ptr<AndroidPermissionState> activePermissionState() {
    std::lock_guard lock(callbackMutex);
    return callbackBackend.lock();
}

std::shared_ptr<AndroidCameraState> activeCameraState() {
    std::lock_guard lock(callbackMutex);
    return callbackCamera;
}

std::shared_ptr<AndroidAudioState> activeAudioState(bool systemAudio) {
    std::lock_guard lock(callbackMutex);
    return systemAudio ? callbackSystemAudio : callbackMicrophone;
}

void clearCameraState(const std::shared_ptr<AndroidCameraState>& state) {
    std::lock_guard lock(callbackMutex);
    if (callbackCamera == state) callbackCamera.reset();
}

void clearAudioState(const std::shared_ptr<AndroidAudioState>& state,
                     bool systemAudio) {
    std::lock_guard lock(callbackMutex);
    auto& callback = systemAudio ? callbackSystemAudio : callbackMicrophone;
    if (callback == state) callback.reset();
}

}  // namespace

std::unique_ptr<capture::IDeviceCaptureBackend> makeAndroidDeviceCaptureBackend() {
    return std::make_unique<AndroidDeviceBackend>();
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMediaPermissionResult(
    JNIEnv*, jclass, jint kind, jlong generation, jboolean granted) {
    if (auto state = creator::app::android::activePermissionState()) {
        state->resolve(kind, static_cast<std::uint64_t>(generation), granted == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeCameraFrame(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint width, jint height, jlong timestampNs) {
    auto state = creator::app::android::activeCameraState();
    if (!state) return;
    auto* bytes = static_cast<std::byte*>(environment->GetDirectBufferAddress(buffer));
    const auto size = environment->GetDirectBufferCapacity(buffer);
    state->frame(static_cast<std::uint64_t>(generation), bytes,
        size < 0 ? 0 : static_cast<std::size_t>(size), width, height, timestampNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMicrophonePcm16(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint sampleCount,
    jint sampleRate, jint channels, jlong timestampNs) {
    auto state = creator::app::android::activeAudioState(false);
    if (!state || sampleCount <= 0) return;
    auto* pcm = static_cast<std::int16_t*>(environment->GetDirectBufferAddress(buffer));
    const auto capacity = environment->GetDirectBufferCapacity(buffer);
    const auto samples = static_cast<std::size_t>(sampleCount);
    state->pcm16(static_cast<std::uint64_t>(generation),
                 capacity < static_cast<jlong>(samples * sizeof(std::int16_t)) ? nullptr : pcm,
                 samples, sampleRate, channels, timestampNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeCameraFailed(
    JNIEnv*, jclass, jlong generation) {
    if (auto state = creator::app::android::activeCameraState()) {
        state->failed(static_cast<std::uint64_t>(generation), "Android Camera2 failed");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMicrophoneFailed(
    JNIEnv*, jclass, jlong generation) {
    if (auto state = creator::app::android::activeAudioState(false)) {
        state->failed(static_cast<std::uint64_t>(generation), "Android AudioRecord failed");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeSystemAudioPcm16(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint sampleCount,
    jint sampleRate, jint channels, jlong timestampNs) {
    auto state = creator::app::android::activeAudioState(true);
    if (!state || sampleCount <= 0) return;
    auto* pcm = static_cast<std::int16_t*>(environment->GetDirectBufferAddress(buffer));
    const auto capacity = environment->GetDirectBufferCapacity(buffer);
    const auto samples = static_cast<std::size_t>(sampleCount);
    state->pcm16(static_cast<std::uint64_t>(generation),
                 capacity < static_cast<jlong>(samples * sizeof(std::int16_t)) ? nullptr : pcm,
                 samples, sampleRate, channels, timestampNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeSystemAudioFailed(
    JNIEnv*, jclass, jlong generation) {
    if (auto state = creator::app::android::activeAudioState(true)) {
        state->failed(static_cast<std::uint64_t>(generation),
                      "Android playback audio capture failed");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeCameraStopped(
    JNIEnv*, jclass, jlong generation) {
    auto state = creator::app::android::activeCameraState();
    if (!state) return;
    state->stopped(static_cast<std::uint64_t>(generation));
    creator::app::android::clearCameraState(state);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeMicrophoneStopped(
    JNIEnv*, jclass, jlong generation) {
    auto state = creator::app::android::activeAudioState(false);
    if (!state) return;
    state->stopped(static_cast<std::uint64_t>(generation));
    creator::app::android::clearAudioState(state, false);
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeSystemAudioStopped(
    JNIEnv*, jclass, jlong generation) {
    auto state = creator::app::android::activeAudioState(true);
    if (!state) return;
    state->stopped(static_cast<std::uint64_t>(generation));
    creator::app::android::clearAudioState(state, true);
}

}  // namespace creator::app::android
