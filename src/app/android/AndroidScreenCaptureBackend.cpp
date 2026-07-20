#include "app/android/AndroidScreenCaptureBackend.h"

#include "capture/AndroidProjectionSession.h"
#include "capture/ScreenCaptureFrameAssembler.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <QGuiApplication>
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QScreen>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
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

    [[nodiscard]] bool acceptFrame(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        if (generation != session_.generation()) return false;
        if (session_.state() == capture::ProjectionSessionState::Starting) {
            return session_.markStreaming(generation);
        }
        return session_.state() == capture::ProjectionSessionState::Streaming;
    }

    [[nodiscard]] bool requestStop(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        return session_.requestStop(generation);
    }

    [[nodiscard]] bool revoke(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        return session_.onProjectionRevoked(generation);
    }

    [[nodiscard]] bool finishStop(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        return session_.markStopped(generation);
    }

    [[nodiscard]] std::optional<capture::ProjectionSessionState> state(
        std::uint64_t generation) const {
        std::lock_guard lock(mutex_);
        if (generation != session_.generation()) return std::nullopt;
        return session_.state();
    }

private:
    mutable std::mutex mutex_;
    capture::AndroidProjectionSession session_;
    capture::IScreenCapturePermission::Completion completion_;
};

std::mutex callbackMutex;
std::weak_ptr<ProjectionConsentState> callbackState;
class ProjectionSourceState;
std::shared_ptr<ProjectionSourceState> callbackSourceState;

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

class ProjectionSourceState final {
public:
    ProjectionSourceState(std::shared_ptr<ProjectionConsentState> consent,
                          std::shared_ptr<capture::IVideoFrameSink> sink)
        : consent_(std::move(consent)), sink_(std::move(sink)) {}

    void begin(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        generation_ = generation;
        acceptingFrames_ = true;
        released_ = false;
        releaseRequested_ = false;
        captureStarted_ = false;
        terminalError_.reset();
        stopCompletions_.clear();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    [[nodiscard]] bool requestStop(capture::IScreenCaptureSource::StopCompletion completion) {
        capture::IScreenCaptureSource::StopCompletion immediate;
        bool requestJavaRelease = false;
        {
            std::lock_guard lock(mutex_);
            acceptingFrames_ = false;
            if (released_ || generation_ == 0) {
                immediate = std::move(completion);
            } else {
                if (completion) stopCompletions_.push_back(std::move(completion));
                if (!releaseRequested_) {
                    const auto current = consent_->state(generation_);
                    if (current == capture::ProjectionSessionState::Starting ||
                        current == capture::ProjectionSessionState::Streaming) {
                        releaseRequested_ = consent_->requestStop(generation_);
                        requestJavaRelease = releaseRequested_;
                    } else if (current == capture::ProjectionSessionState::Stopping ||
                               current == capture::ProjectionSessionState::Revoked) {
                        releaseRequested_ = true;
                    } else {
                        released_ = true;
                        immediate = takeLastCompletion();
                    }
                }
            }
        }
        if (immediate) immediate(core::ok());
        return requestJavaRelease;
    }

    void failStop(core::AppError error) {
        std::vector<capture::IScreenCaptureSource::StopCompletion> completions;
        {
            std::lock_guard lock(mutex_);
            acceptingFrames_ = false;
            released_ = true;
            static_cast<void>(consent_->finishStop(generation_));
            completions = std::move(stopCompletions_);
        }
        for (auto& completion : completions) {
            if (completion) completion(error);
        }
    }

    void abortStart() {
        std::lock_guard lock(mutex_);
        acceptingFrames_ = false;
        static_cast<void>(consent_->revoke(generation_));
        static_cast<void>(consent_->finishStop(generation_));
        released_ = true;
    }

    [[nodiscard]] capture::CaptureStats stats() const noexcept {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    [[nodiscard]] bool onProjectionFrame(std::uint64_t generation, const std::byte* bytes, std::size_t size,
                                         std::uint32_t width, std::uint32_t height,
                                         std::uint32_t rowStride, std::uint32_t pixelStride,
                                         std::int64_t timestampNs) noexcept {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || !acceptingFrames_) {
            ++stats_.ignoredFrames;
            return false;
        }
        if (!bytes || width == 0 || height == 0 || pixelStride < 4 ||
            rowStride < width * pixelStride ||
            size < static_cast<std::size_t>(rowStride) * height) {
            ++stats_.invalidFrames;
            return false;
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
        if (!consent_->acceptFrame(generation)) {
            ++stats_.ignoredFrames;
            return false;
        }
        if (!captureStarted_) { captureStarted_ = true; sink_->onCaptureStarted(); }
        sink_->onVideoFrame(std::move(*assembled.value())); ++stats_.receivedFrames; return true;
    }

    void onProjectionRevoked(std::uint64_t generation) noexcept {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || !consent_->revoke(generation)) return;
        acceptingFrames_ = false;
        terminalError_ = core::AppError{core::ErrorCode::InvalidState,
                                        "Android revoked screen recording permission"};
        sink_->onCaptureError(*terminalError_);
    }

    void onProjectionReleased(std::uint64_t generation, bool revoked) {
        std::vector<capture::IScreenCaptureSource::StopCompletion> completions;
        std::optional<core::AppError> resultError;
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_ || released_) return;
            if (revoked && consent_->revoke(generation)) {
                acceptingFrames_ = false;
                terminalError_ = core::AppError{
                    core::ErrorCode::InvalidState,
                    "Android revoked screen recording permission"};
                sink_->onCaptureError(*terminalError_);
            }
            if (!consent_->finishStop(generation)) return;
            acceptingFrames_ = false;
            released_ = true;
            resultError = terminalError_;
            completions = std::move(stopCompletions_);
        }
        for (auto& completion : completions) {
            if (!completion) continue;
            if (resultError) completion(*resultError);
            else completion(core::ok());
        }
    }

private:
    capture::IScreenCaptureSource::StopCompletion takeLastCompletion() {
        if (stopCompletions_.empty()) return {};
        auto completion = std::move(stopCompletions_.back());
        stopCompletions_.pop_back();
        return completion;
    }

private:
    std::shared_ptr<ProjectionConsentState> consent_;
    std::shared_ptr<capture::IVideoFrameSink> sink_;
    capture::ScreenCaptureFrameAssembler assembler_;
    mutable std::mutex mutex_;
    capture::CaptureStats stats_{};
    std::uint64_t generation_{};
    bool acceptingFrames_{};
    bool releaseRequested_{};
    bool released_{true};
    bool captureStarted_{};
    std::optional<core::AppError> terminalError_;
    std::vector<capture::IScreenCaptureSource::StopCompletion> stopCompletions_;
};

std::shared_ptr<ProjectionSourceState> activeProjectionSource() {
    std::lock_guard lock(callbackMutex);
    return callbackSourceState;
}

void clearProjectionSource(const std::shared_ptr<ProjectionSourceState>& source) {
    std::lock_guard lock(callbackMutex);
    if (callbackSourceState == source) callbackSourceState.reset();
}

class AndroidScreenCaptureSource final : public capture::IScreenCaptureSource {
public:
    AndroidScreenCaptureSource(std::shared_ptr<ProjectionConsentState> state,
                               std::shared_ptr<capture::IVideoFrameSink> sink)
        : consent_(std::move(state)),
          sourceState_(std::make_shared<ProjectionSourceState>(consent_, std::move(sink))),
          id_(domain::SourceId::create("android-screen").value()) {}
    ~AndroidScreenCaptureSource() override { static_cast<void>(stop()); }
    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return "Android display"; }
    [[nodiscard]] core::Result<void> start(const capture::CaptureConfig& config) override {
        const auto generation = consent_->approvedGeneration();
        if (!generation) return core::AppError{core::ErrorCode::InvalidState,
                                                "Android screen recording permission is not active"};
        sourceState_->begin(*generation);
        {
            std::lock_guard lock(callbackMutex);
            callbackSourceState = sourceState_;
        }
        QJniObject::callStaticMethod<void>(kActivityClass, "startProjection", "(JII)V",
                                            static_cast<jlong>(*generation),
                                            static_cast<jint>(config.targetWidth),
                                            static_cast<jint>(config.targetHeight));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            sourceState_->abortStart();
            clearProjectionSource(sourceState_);
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Android could not start MediaProjection"};
        }
        return core::ok();
    }
    [[nodiscard]] core::Result<void> stop() override {
        return requestStop({});
    }
    void stopAsync(StopCompletion completion) override {
        static_cast<void>(requestStop(std::move(completion)));
    }
    [[nodiscard]] capture::CaptureStats stats() const noexcept override {
        return sourceState_->stats();
    }

private:
    [[nodiscard]] core::Result<void> requestStop(StopCompletion completion) {
        const auto generation = sourceState_->generation();
        if (!sourceState_->requestStop(std::move(completion))) return core::ok();
        QJniObject::callStaticMethod<void>(kActivityClass, "stopProjection", "(J)V",
                                            static_cast<jlong>(generation));
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            auto error = core::AppError{core::ErrorCode::IoFailure,
                                        "Android could not stop MediaProjection"};
            sourceState_->failStop(error);
            clearProjectionSource(sourceState_);
            return error;
        }
        return core::ok();
    }

    std::shared_ptr<ProjectionConsentState> consent_;
    std::shared_ptr<ProjectionSourceState> sourceState_;
    domain::SourceId id_;
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

void queueOnApplicationThread(std::function<void()> callback) {
    auto* context = QCoreApplication::instance();
    if (!context) {
        callback();
        return;
    }
    QMetaObject::invokeMethod(context, std::move(callback), Qt::QueuedConnection);
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
    auto source = creator::app::android::activeProjectionSource();
    if (!source) return JNI_FALSE;
    auto* bytes = static_cast<std::byte*>(environment->GetDirectBufferAddress(buffer));
    const auto size = environment->GetDirectBufferCapacity(buffer);
    return source->onProjectionFrame(
               static_cast<std::uint64_t>(generation), bytes,
               size < 0 ? 0 : static_cast<std::size_t>(size), width, height,
               rowStride, pixelStride, timestampNs) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionRevoked(
    JNIEnv*, jclass, jlong generation) {
    auto source = creator::app::android::activeProjectionSource();
    if (!source) return;
    creator::app::android::queueOnApplicationThread(
        [source = std::move(source), generation = static_cast<std::uint64_t>(generation)] {
            source->onProjectionRevoked(generation);
        });
}

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionReleased(
    JNIEnv*, jclass, jlong generation, jboolean revoked) {
    auto source = creator::app::android::activeProjectionSource();
    if (!source) return;
    creator::app::android::queueOnApplicationThread(
        [source = std::move(source), generation = static_cast<std::uint64_t>(generation),
         revoked = revoked == JNI_TRUE] {
            source->onProjectionReleased(generation, revoked);
            creator::app::android::clearProjectionSource(source);
        });
}

}  // namespace creator::app::android

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionResult(
    JNIEnv*, jclass, jlong generation, jboolean granted) {
    if (auto state = creator::app::android::activeCallbackState()) {
        state->resolve(static_cast<std::uint64_t>(generation), granted == JNI_TRUE);
    }
}
