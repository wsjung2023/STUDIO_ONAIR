#include "app/android/AndroidScreenCaptureBackend.h"

#include "capture/AndroidProjectionSession.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QScreen>

#include <cstdint>
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

class AndroidScreenSourceFactory final : public capture::IScreenCaptureSourceFactory {
public:
    [[nodiscard]] core::Result<std::unique_ptr<capture::IScreenCaptureSource>> create(
        const domain::CaptureTargetId&, std::shared_ptr<capture::IVideoFrameSink>) override {
        return core::AppError{
            core::ErrorCode::InvalidState,
            "Android screen capture is awaiting the MediaProjection frame adapter"};
    }
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
            .sourceFactory = std::make_unique<AndroidScreenSourceFactory>()};
}

}  // namespace creator::app::android

extern "C" JNIEXPORT void JNICALL
Java_com_studioonair_creatorstudio_CreatorStudioActivity_nativeProjectionResult(
    JNIEnv*, jclass, jlong generation, jboolean granted) {
    if (auto state = creator::app::android::activeCallbackState()) {
        state->resolve(static_cast<std::uint64_t>(generation), granted == JNI_TRUE);
    }
}
