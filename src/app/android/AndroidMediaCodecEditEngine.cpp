#include "app/android/AndroidMediaCodecEditEngine.h"

#include "app/android/AndroidTimelineExportPlan.h"
#include "core/AppError.h"
#include "edit_engine/IRenderJobLifecycle.h"
#include "project_store/PersistentRenderJobLifecycle.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/RenderJobRecovery.h"
#include "project_store/SqliteRenderJobStore.h"

#include <QJniEnvironment>
#include <QJniObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace creator::app::android {
namespace {

constexpr const char* kActivityClass =
    "com/studioonair/creatorstudio/CreatorStudioActivity";
constexpr jint kJavaPending = 0;
constexpr jint kJavaRunning = 1;
constexpr jint kJavaCompleted = 4;
constexpr jint kJavaFailed = 5;
constexpr jint kJavaCancelled = 6;

core::AppError unsupported() {
    return {core::ErrorCode::InvalidState,
            "Android MediaCodec engine only supports export"};
}

core::AppError ioError(std::string message) {
    return {core::ErrorCode::IoFailure, std::move(message)};
}

core::Result<void> publishAtomically(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    edit_engine::RenderOverwritePolicy policy) {
    std::error_code error;
    if (policy == edit_engine::RenderOverwritePolicy::FailIfExists &&
        std::filesystem::exists(destination, error)) {
        return core::AppError{core::ErrorCode::AlreadyExists,
                              "export destination already exists"};
    }
    if (error) return ioError("export destination could not be inspected");
    std::filesystem::rename(partial, destination, error);
    if (error) return ioError("export artifact could not be atomically published");
    return core::ok();
}

void removeFile(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

std::string javaError(jlong handle) {
    auto result = QJniObject::callStaticObjectMethod(
        kActivityClass, "timelineExportError", "(J)Ljava/lang/String;", handle);
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions()) {
        return "Android timeline exporter status failed";
    }
    return result.toString().toStdString();
}

void cancelJava(jlong handle) noexcept {
    if (handle <= 0) return;
    QJniObject::callStaticMethod<void>(kActivityClass, "cancelTimelineExport",
                                       "(J)V", handle);
    QJniEnvironment environment;
    static_cast<void>(environment.checkAndClearExceptions());
}

void releaseJava(jlong handle) noexcept {
    if (handle <= 0) return;
    QJniObject::callStaticMethod<void>(kActivityClass, "releaseTimelineExport",
                                       "(J)V", handle);
    QJniEnvironment environment;
    static_cast<void>(environment.checkAndClearExceptions());
}

class AndroidMediaCodecRenderJob final : public edit_engine::IRenderJob {
public:
    AndroidMediaCodecRenderJob(
        edit_engine::RenderRequest request, std::filesystem::path partial,
        core::DurationNs duration,
        std::shared_ptr<edit_engine::IRenderJobLifecycle> lifecycle,
        jlong handle)
        : request_(std::move(request)),
          partial_(std::move(partial)),
          duration_(duration),
          lifecycle_(std::move(lifecycle)),
          handle_(handle),
          progress_(edit_engine::RenderProgress::create(
                        edit_engine::RenderJobState::Pending, 0.0,
                        core::TimestampNs{}, duration_)
                        .value()) {}

    ~AndroidMediaCodecRenderJob() override {
        std::lock_guard lock(mutex_);
        if (!terminal_) {
            cancelJava(handle_);
            removeFile(partial_);
            finishLocked(edit_engine::RenderJobState::Cancelled,
                         "Android export owner was released");
        }
        releaseJava(std::exchange(handle_, 0));
    }

    core::Result<edit_engine::RenderProgress> progress() const override {
        std::lock_guard lock(mutex_);
        if (terminal_) return progress_;

        const auto state = QJniObject::callStaticMethod<jint>(
            kActivityClass, "timelineExportState", "(J)I", handle_);
        const auto fraction = QJniObject::callStaticMethod<jdouble>(
            kActivityClass, "timelineExportProgress", "(J)D", handle_);
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions() || !std::isfinite(fraction) ||
            fraction < 0.0 || fraction > 1.0) {
            failLocked("Android timeline exporter returned invalid progress");
            return progress_;
        }
        if (state == kJavaFailed) {
            auto diagnostic = javaError(handle_);
            failLocked(diagnostic.empty() ? "Android timeline export failed"
                                          : std::move(diagnostic));
            return progress_;
        }
        if (state == kJavaCancelled) {
            removeFile(partial_);
            finishLocked(edit_engine::RenderJobState::Cancelled,
                         "Android timeline export cancelled");
            return progress_;
        }
        if (state == kJavaCompleted) {
            completeLocked();
            return progress_;
        }
        if (state != kJavaPending && state != kJavaRunning) {
            failLocked("Android timeline exporter returned an unknown state");
            return progress_;
        }

        const auto bounded = std::min(fraction, 0.998);
        const auto rendered = core::TimestampNs{core::DurationNs{
            static_cast<core::DurationNs::rep>(
                static_cast<double>(duration_.count()) * bounded)}};
        const auto visibleState = cancellationRequested_
                                      ? edit_engine::RenderJobState::Cancelling
                                      : state == kJavaPending
                                            ? edit_engine::RenderJobState::Pending
                                            : edit_engine::RenderJobState::Running;
        auto next = edit_engine::RenderProgress::create(
            visibleState,
            visibleState == edit_engine::RenderJobState::Pending ? 0.0 : bounded,
            visibleState == edit_engine::RenderJobState::Pending
                ? core::TimestampNs{}
                : rendered,
            duration_);
        if (!next.hasValue()) {
            failLocked(next.error().message());
            return progress_;
        }
        progress_ = std::move(next).value();
        if (lifecycle_ && visibleState != edit_engine::RenderJobState::Pending) {
            auto advanced = lifecycle_->advance(request_.jobId(), progress_);
            if (!advanced.hasValue()) failLocked(advanced.error().message());
        }
        return progress_;
    }

    core::Result<void> cancel() override {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "render job is already terminal"};
        }
        if (progress_.state() == edit_engine::RenderJobState::Publishing) {
            return core::AppError{
                core::ErrorCode::InvalidState,
                "render publication has crossed the cancellation boundary"};
        }
        cancellationRequested_ = true;
        cancelJava(handle_);
        auto next = edit_engine::RenderProgress::create(
            edit_engine::RenderJobState::Cancelling, progress_.fraction(),
            progress_.renderedThrough(), duration_);
        if (!next.hasValue()) return next.error();
        progress_ = std::move(next).value();
        return core::ok();
    }

    std::string diagnostic() const override {
        std::lock_guard lock(mutex_);
        return diagnostic_;
    }

private:
    void failLocked(std::string diagnostic) const {
        cancelJava(handle_);
        removeFile(partial_);
        finishLocked(edit_engine::RenderJobState::Failed,
                     std::move(diagnostic));
    }

    void completeLocked() const {
        std::error_code error;
        const auto bytes = std::filesystem::file_size(partial_, error);
        if (error || bytes == 0) {
            failLocked("Android MediaMuxer produced an empty export");
            return;
        }
        auto publishing = edit_engine::RenderProgress::create(
            edit_engine::RenderJobState::Publishing, 0.999,
            core::TimestampNs{duration_}, duration_);
        if (!publishing.hasValue()) {
            failLocked(publishing.error().message());
            return;
        }
        progress_ = std::move(publishing).value();
        if (lifecycle_) {
            auto prepared = lifecycle_->preparePublication(
                request_.jobId(), partial_, progress_);
            if (!prepared.hasValue()) {
                failLocked(prepared.error().message());
                return;
            }
        }
        auto published = publishAtomically(partial_, request_.destination(),
                                           request_.overwritePolicy());
        if (!published.hasValue()) {
            failLocked(published.error().message());
            return;
        }
        finishLocked(edit_engine::RenderJobState::Completed, {});
    }

    void finishLocked(edit_engine::RenderJobState state,
                      std::string diagnostic) const {
        if (terminal_) return;
        diagnostic_ = std::move(diagnostic);
        const bool completed = state == edit_engine::RenderJobState::Completed;
        auto finished = edit_engine::RenderProgress::create(
            state, completed ? 1.0 : std::min(progress_.fraction(), 0.998),
            completed ? core::TimestampNs{duration_}
                      : progress_.renderedThrough(),
            duration_);
        if (lifecycle_) {
            auto recorded = lifecycle_->finish(request_.jobId(), state,
                                                diagnostic_);
            if (!recorded.hasValue()) {
                if (!diagnostic_.empty()) diagnostic_ += "; ";
                diagnostic_ += recorded.error().message();
                if (completed) {
                    auto failed = edit_engine::RenderProgress::create(
                        edit_engine::RenderJobState::Failed, 0.999,
                        core::TimestampNs{duration_}, duration_);
                    if (failed.hasValue()) progress_ = std::move(failed).value();
                    static_cast<void>(lifecycle_->finish(
                        request_.jobId(), edit_engine::RenderJobState::Failed,
                        diagnostic_));
                    terminal_ = true;
                    releaseJava(std::exchange(handle_, 0));
                    return;
                }
            }
        }
        if (finished.hasValue()) progress_ = std::move(finished).value();
        terminal_ = true;
        releaseJava(std::exchange(handle_, 0));
    }

    edit_engine::RenderRequest request_;
    std::filesystem::path partial_;
    core::DurationNs duration_;
    std::shared_ptr<edit_engine::IRenderJobLifecycle> lifecycle_;
    mutable std::mutex mutex_;
    mutable jlong handle_{};
    mutable edit_engine::RenderProgress progress_;
    mutable std::string diagnostic_;
    mutable bool cancellationRequested_{};
    mutable bool terminal_{};
};

}  // namespace

core::Result<void> AndroidMediaCodecEditEngine::load(
    const edit_engine::TimelineSnapshot&) {
    return unsupported();
}
core::Result<void> AndroidMediaCodecEditEngine::update(
    const edit_engine::TimelineChangeSet&) {
    return unsupported();
}
core::Result<void> AndroidMediaCodecEditEngine::play() { return unsupported(); }
core::Result<void> AndroidMediaCodecEditEngine::pause() { return unsupported(); }
core::Result<void> AndroidMediaCodecEditEngine::seek(core::TimestampNs) {
    return unsupported();
}
core::Result<edit_engine::PreviewFrame>
AndroidMediaCodecEditEngine::requestFrame(core::TimestampNs) {
    return unsupported();
}

core::Result<std::unique_ptr<edit_engine::IRenderJob>>
AndroidMediaCodecEditEngine::render(const edit_engine::RenderRequest& request) {
    project_store::ProjectPackageStore packages;
    auto opened = packages.open(request.snapshot().mediaRoot);
    if (!opened.hasValue()) return opened.error();
    if (opened.value().package.manifest.projectId != request.projectId()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "export request project identity changed"};
    }
    const auto lease = opened.value().databaseIdentityLease;
    if (!lease) return ioError("validated export database identity is missing");
    auto sqlite = project_store::SqliteRenderJobStore::open(
        opened.value().databasePath, request.projectId(),
        [lease] { return lease->verifyCurrentIdentity(); });
    if (!sqlite.hasValue()) return sqlite.error();
    auto store = std::make_shared<project_store::SqliteRenderJobStore>(
        std::move(sqlite).value());
    auto recovered = project_store::RenderJobRecovery::recoverAll(
        *store, core::Utc::now());
    if (!recovered.hasValue()) return recovered.error();
    auto lifecycle =
        std::make_shared<project_store::PersistentRenderJobLifecycle>(store);
    const auto partial = request.destination().parent_path() /
                         (".creator-studio-" + request.jobId().value() +
                          ".partial.mp4");
    auto plan = buildAndroidTimelineExportPlan(request, partial);
    if (!plan.hasValue()) return plan.error();
    auto begun = lifecycle->begin(request, partial, plan.value().duration);
    if (!begun.hasValue()) return begun.error();
    auto selected = lifecycle->encoderSelected(
        request.jobId(),
        {.attemptedEncoders = "android-mediacodec-h264+aac",
         .selectedEncoder = "android-mediacodec-h264+aac",
         .fallbackReason = {}});
    if (!selected.hasValue()) {
        static_cast<void>(lifecycle->finish(
            request.jobId(), edit_engine::RenderJobState::Failed,
            selected.error().message()));
        return selected.error();
    }

    const auto spec = QJniObject::fromString(QString::fromUtf8(plan.value().json));
    const auto handle = QJniObject::callStaticMethod<jlong>(
        kActivityClass, "startTimelineExport", "(Ljava/lang/String;)J",
        spec.object<jstring>());
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions() || handle <= 0) {
        removeFile(partial);
        static_cast<void>(lifecycle->finish(
            request.jobId(), edit_engine::RenderJobState::Failed,
            "Android timeline exporter could not start"));
        return ioError("Android timeline exporter could not start");
    }
    std::unique_ptr<edit_engine::IRenderJob> result =
        std::make_unique<AndroidMediaCodecRenderJob>(
            request, partial, plan.value().duration, std::move(lifecycle),
            handle);
    return result;
}

}  // namespace creator::app::android
