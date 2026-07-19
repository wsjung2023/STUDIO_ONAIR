#include "app/android/AndroidScreenCaptureBackend.h"

#include "capture/AndroidProjectionSession.h"
#include "capture/ScreenCaptureFrameAssembler.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QScreen>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace creator::app::android {
namespace {

constexpr const char* kActivityClass =
    "com/studioonair/creatorstudio/CreatorStudioActivity";

class ProjectionConsentState final {
public:
    [[nodiscard]] std::optional<std::uint64_t> approvedGeneration() const {
        std::lock_guard lock(mutex_);
        return session_.state() == capture::ProjectionSessionState::Starting
                   ? std::optional<std::uint64_t>{session_.generation()}
                   : std::nullopt;
    }
    [[nodiscard]] capture::ScreenCapturePermissionStatus status() const noexcept {
        std::lock_guard lock(mutex_);
        switch (session_.state()) {
            case capture::ProjectionSessionState::Starting:
            case capture::ProjectionSessionState::Streaming:
                return capture::ScreenCapturePermissionStatus::Granted;
            case capture::ProjectionSessionState::Stopped:
                return capture::ScreenCapturePermissionStatus::Denied;
            default: return capture::ScreenCapturePermissionStatus::Unknown;
        }
    }

    [[nodiscard]] std::optional<std::uint64_t> begin(
        capture::IScreenCapturePermission::Completion completion) {
        std::lock_guard lock(mutex_);
        if (completion_) return std::nullopt;
        const auto generation = session_.beginProjectionRequest();
        completion_ = std::move(completion);
        return generation;
    }

    void resolve(std::uint64_t generation, bool granted) {
        capture::IScreenCapturePermission::Completion completion;
        capture::ScreenCapturePermissionStatus status =
            capture::ScreenCapturePermissionStatus::Denied;
        {
            std::lock_guard lock(mutex_);
            if (!completion_ || generation != session_.generation()) return;
            const bool transitioned = granted ? session_.approveProjection(generation)
                                              : session_.denyProjection(generation);
            if (!transitioned) return;
            status = granted ? capture::ScreenCapturePermissionStatus::Granted
                             : capture::ScreenCapturePermissionStatus::Denied;
            completion = std::move(completion_);
        }
        if (completion) completion(status);
    }

private:
    mutable std::mutex mutex_;
    capture::AndroidProjectionSession session_;
    capture::IScreenCapturePermission::Completion completion_;
};

std::mutex callbackMutex;
std::weak_ptr<ProjectionConsentState> callbackState;
class AndroidScreenCaptureSource;
AndroidScreenCaptureSource* callbackSource{};

class AndroidScreenPermission final : public capture::IScreenCapturePermission {
public:
    explicit AndroidScreenPermission(std::shared_ptr<ProjectionConsentState> state)
        : state_(std::move(state)) {}

    ~AndroidScreenPermission() override {
        std::lock_guard lock(callbackMutex);
        callbackState.reset();
    }

    [[nodiscard]] capture::ScreenCapturePermissionStatus status() const noexcept override {
        return state_->status();
    }

    void request(Completion completion) override {
        const auto generation = state_->begin(std::move(completion));
        if (!generation) return;

        QJniObject::callStaticMethod<void>(kActivityClass, "requestProjection", "(J)V",
                                            static_cast<jlong>(*generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) state_->resolve(*generation, false);
    }

private:
    std::shared_ptr<ProjectionConsentState> state_;
};

class AndroidScreenDiscovery final : public capture::IScreenCaptureDiscovery {
public:
    void enumerate(Completion completion) override {
        auto id = domain::CaptureTargetId::create("android-mediaprojection-display");
        if (!id.hasValue()) {
            if (completion) completion(id.error());
            return;
        }

        const auto* screen = QGuiApplication::primaryScreen();
        const QSize logicalSize = screen ? screen->size() : QSize{1, 1};
        const qreal scale = screen ? screen->devicePixelRatio() : 1.0;
        const auto width = static_cast<std::uint32_t>(
            std::max(1, qRound(static_cast<qreal>(logicalSize.width()) * scale)));
        const auto height = static_cast<std::uint32_t>(
            std::max(1, qRound(static_cast<qreal>(logicalSize.height()) * scale)));
        auto target = capture::ScreenCaptureTarget::create(
            std::move(id).value(), capture::ScreenCaptureTargetKind::Display,
            "Android display", std::nullopt, width, height);
        if (!target.hasValue()) {
            if (completion) completion(target.error());
            return;
        }
        std::vector<capture::ScreenCaptureTarget> targets;
        targets.push_back(std::move(target).value());
        if (completion) completion(std::move(targets));
    }
};

class AndroidScreenCaptureSource final : public capture::IScreenCaptureSource {
public:
    AndroidScreenCaptureSource(std::shared_ptr<ProjectionConsentState> state,
                               std::shared_ptr<capture::IVideoFrameSink> sink)
        : state_(std::move(state)), sink_(std::move(sink)),
          id_(domain::SourceId::create("android-screen").value()) {}
    ~AndroidScreenCaptureSource() override { static_cast<void>(stop()); }
    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return "Android display"; }
    [[nodiscard]] core::Result<void> start(const capture::CaptureConfig& config) override {
        const auto generation = state_->approvedGeneration();
        if (!generation) return core::AppError{core::ErrorCode::InvalidState,
                                                "Android screen recording permission is not active"};
        generation_ = *generation;
        {
            std::lock_guard lock(callbackMutex);
            callbackSource = this;
        }
        QJniObject::callStaticMethod<void>(kActivityClass, "startProjection", "(JII)V",
                                            static_cast<jlong>(generation_),
                                            static_cast<jint>(config.targetWidth),
                                            static_cast<jint>(config.targetHeight));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) return core::AppError{
            core::ErrorCode::IoFailure, "Android could not start MediaProjection"};
        return core::ok();
    }
    [[nodiscard]] core::Result<void> stop() override {
        QJniObject::callStaticMethod<void>(kActivityClass, "stopProjection", "(J)V",
                                            static_cast<jlong>(generation_));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) return core::AppError{
            core::ErrorCode::IoFailure, "Android could not stop MediaProjection"};
        std::lock_guard lock(callbackMutex);
        if (callbackSource == this) callbackSource = nullptr;
        return core::ok();
    }
    void stopAsync(StopCompletion completion) override {
        const auto result = stop();
        if (completion) completion(result);
    }
    [[nodiscard]] capture::CaptureStats stats() const noexcept override { return stats_; }
    [[nodiscard]] bool onProjectionFrame(std::uint64_t generation, const std::byte* bytes, std::size_t size,
                                         std::uint32_t width, std::uint32_t height,
                                         std::uint32_t rowStride, std::uint32_t pixelStride,
                                         std::int64_t timestampNs) noexcept {
        if (generation != generation_ || !bytes || width == 0 || height == 0 || pixelStride < 4 ||
            rowStride < width * pixelStride || size < static_cast<std::size_t>(rowStride) * height) {
            ++stats_.invalidFrames; return false;
        }
        auto pixels = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            const auto source = static_cast<std::size_t>(y) * rowStride + x * pixelStride;
            const auto target = (static_cast<std::size_t>(y) * width + x) * 4;
            (*pixels)[target] = bytes[source + 2]; (*pixels)[target + 1] = bytes[source + 1];
            (*pixels)[target + 2] = bytes[source]; (*pixels)[target + 3] = bytes[source + 3];
        }
        auto assembled = assembler_.assemble(capture::NativeScreenFrame{
            .timestamp = {timestampNs, 1'000'000'000}, .width = width, .height = height,
            .pixelFormat = media::PixelFormat::Bgra8, .platformHandle = std::move(pixels)});
        if (!assembled.hasValue() || !assembled.value()) { ++stats_.invalidFrames; return false; }
        if (!started_) { started_ = true; sink_->onCaptureStarted(); }
        sink_->onVideoFrame(std::move(*assembled.value())); ++stats_.receivedFrames; return true;
    }
    void onProjectionRevoked(std::uint64_t generation) noexcept {
        if (generation != generation_) return;
        sink_->onCaptureError(core::AppError{core::ErrorCode::InvalidState,
                                             "Android revoked screen recording permission"});
    }
private:
    std::shared_ptr<ProjectionConsentState> state_; std::shared_ptr<capture::IVideoFrameSink> sink_;
    domain::SourceId id_; capture::ScreenCaptureFrameAssembler assembler_; capture::CaptureStats stats_{};
    std::uint64_t generation_{}; bool started_{};
};

class AndroidScreenSourceFactory final : public capture::IScreenCaptureSourceFactory {
public:
    explicit AndroidScreenSourceFactory(std::shared_ptr<ProjectionConsentState> state) : state_(std::move(state)) {}
    [[nodiscard]] core::Result<std::unique_ptr<capture::IScreenCaptureSource>> create(
        const domain::CaptureTargetId&, std::shared_ptr<capture::IVideoFrameSink> sink) override {
        if (!sink) return core::AppError{core::ErrorCode::InvalidArgument, "screen capture sink is required"};
        return std::unique_ptr<capture::IScreenCaptureSource>{
            std::make_unique<AndroidScreenCaptureSource>(state_, std::move(sink))};
    }
private: std::shared_ptr<ProjectionConsentState> state_;
};

std::shared_ptr<ProjectionConsentState> activeCallbackState() {
    std::lock_guard lock(callbackMutex);
    return callbackState.lock();
}

}  // namespace

AndroidScreenCaptureBackend makeAndroidScreenCaptureBackend() {
    auto state = std::make_shared<ProjectionConsentState>();
    {
        std::lock_guard lock(callbackMutex);
        callbackState = state;
    }
    return {.permission = std::make_unique<AndroidScreenPermission>(state),
            .discovery = std::make_unique<AndroidScreenDiscovery>(),
            .sourceFactory = std::make_unique<AndroidScreenSourceFactory>(std::move(state))};
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionFrame(
    JNIEnv* environment, jclass, jlong generation, jobject buffer, jint width, jint height,
    jint rowStride, jint pixelStride, jlong timestampNs) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (!creator::app::android::callbackSource) return JNI_FALSE;
    auto* bytes = static_cast<std::byte*>(environment->GetDirectBufferAddress(buffer));
    const auto size = environment->GetDirectBufferCapacity(buffer);
    return creator::app::android::callbackSource->onProjectionFrame(
               static_cast<std::uint64_t>(generation), bytes,
               size < 0 ? 0 : static_cast<std::size_t>(size), width, height,
               rowStride, pixelStride, timestampNs) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionRevoked(
    JNIEnv*, jclass, jlong generation) {
    std::lock_guard lock(creator::app::android::callbackMutex);
    if (creator::app::android::callbackSource) {
        creator::app::android::callbackSource->onProjectionRevoked(
            static_cast<std::uint64_t>(generation));
    }
}

}  // namespace creator::app::android

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionResult(
    JNIEnv*, jclass, jlong generation, jboolean granted) {
    if (auto state = creator::app::android::activeCallbackState()) {
        state->resolve(static_cast<std::uint64_t>(generation), granted == JNI_TRUE);
    }
}
