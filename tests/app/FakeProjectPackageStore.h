#pragma once

#include "project_store/IProjectPackageStore.h"

#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"

#include <QThread>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace creator::app::test {

class FakeProjectPackageStore final : public project_store::IProjectPackageStore {
public:
    void setOpenResult(project_store::OpenProjectResult value) {
        std::scoped_lock lock{mutex_};
        openResult_ = std::move(value);
    }

    void holdNextCall() {
        std::scoped_lock lock{mutex_};
        release_ = std::make_shared<std::promise<void>>();
        held_ = release_->get_future().share();
        enteredSignal_ = std::make_shared<std::promise<void>>();
        entered_ = enteredSignal_->get_future().share();
        holdNext_ = true;
    }

    [[nodiscard]] bool waitUntilHeldCallEntered() const {
        std::shared_future<void> entered;
        {
            std::scoped_lock lock{mutex_};
            entered = entered_;
        }
        return entered.valid() && entered.wait_for(std::chrono::seconds{3}) ==
                                      std::future_status::ready;
    }

    void releaseHeldCall() {
        std::shared_ptr<std::promise<void>> release;
        {
            std::scoped_lock lock{mutex_};
            release = release_;
        }
        if (release) release->set_value();
    }

    [[nodiscard]] Qt::HANDLE lastThreadId() const noexcept { return lastThreadId_.load(); }

    void failNextMarkSegmentReady(core::AppError error) {
        std::scoped_lock lock{mutex_};
        markSegmentReadyError_ = std::move(error);
    }

    core::Result<project_store::OpenProjectResult> create(
        const std::filesystem::path& path, const std::string& name) override {
        observeAndMaybeHold();
        return makeResult(path, name);
    }

    core::Result<project_store::OpenProjectResult> open(
        const std::filesystem::path& path) override {
        observeAndMaybeHold();
        std::scoped_lock lock{mutex_};
        if (openResult_.has_value()) return *openResult_;
        return makeResult(path, "Opened");
    }

    core::Result<void> beginRecording(const std::filesystem::path&,
                                      const domain::SessionId&, core::TimestampNs,
                                      const core::Utc&) override {
        observeAndMaybeHold();
        return core::ok();
    }
    core::Result<void> completeRecording(const std::filesystem::path&,
                                         const domain::RecordingSession&,
                                         const core::Utc&) override {
        observeAndMaybeHold();
        return core::ok();
    }
    core::Result<void> abortRecording(const std::filesystem::path&,
                                      const domain::SessionId&, std::string_view,
                                      const core::Utc&) override {
        observeAndMaybeHold();
        return core::ok();
    }
    core::Result<void> beginSegment(const std::filesystem::path&,
                                    const domain::SessionId&,
                                    const domain::SegmentInfo&) override {
        observeAndMaybeHold();
        return core::ok();
    }
    core::Result<void> markSegmentReady(const std::filesystem::path&,
                                        const domain::SessionId&,
                                        const domain::SegmentInfo&) override {
        observeAndMaybeHold();
        std::scoped_lock lock{mutex_};
        if (markSegmentReadyError_) {
            auto error = std::move(*markSegmentReadyError_);
            markSegmentReadyError_.reset();
            return error;
        }
        return core::ok();
    }
    core::Result<void> markSegmentFailed(const std::filesystem::path&,
                                         const domain::SessionId&,
                                         const domain::SourceId&, std::uint64_t) override {
        observeAndMaybeHold();
        return core::ok();
    }
    core::Result<project_store::RecoveryResult> recover(
        const std::filesystem::path&, const domain::SessionId& sessionId,
        const core::Utc&) override {
        observeAndMaybeHold();
        return project_store::RecoveryResult{.sessionId = sessionId};
    }

private:
    static domain::ProjectManifest manifest(std::string name) {
        const auto now = core::Utc::parseRfc3339("2026-07-16T12:00:00Z").value();
        return domain::ProjectManifest{
            .schemaVersion = 1,
            .projectId = domain::ProjectId::create(
                             "123e4567-e89b-42d3-a456-426614174000")
                             .value(),
            .name = std::move(name),
            .createdAt = now,
            .updatedAt = now,
            .canvas = {},
            .database = "project.db",
            .directories = {},
            .requiredFeatures = {},
        };
    }

    static project_store::OpenProjectResult makeResult(
        const std::filesystem::path& path, std::string name) {
        return project_store::OpenProjectResult{
            .package = project_store::ProjectPackage{.path = path,
                                                      .manifest = manifest(std::move(name))},
            .recoveryCandidates = {},
        };
    }

    void observeAndMaybeHold() {
        lastThreadId_.store(QThread::currentThreadId());
        std::shared_future<void> held;
        std::shared_ptr<std::promise<void>> enteredSignal;
        {
            std::scoped_lock lock{mutex_};
            if (holdNext_) {
                holdNext_ = false;
                held = held_;
                enteredSignal = enteredSignal_;
            }
        }
        if (enteredSignal) enteredSignal->set_value();
        if (held.valid()) held.wait();
    }

    mutable std::mutex mutex_;
    std::atomic<Qt::HANDLE> lastThreadId_{nullptr};
    std::optional<project_store::OpenProjectResult> openResult_;
    std::optional<core::AppError> markSegmentReadyError_;
    std::shared_ptr<std::promise<void>> release_;
    std::shared_future<void> held_;
    std::shared_ptr<std::promise<void>> enteredSignal_;
    std::shared_future<void> entered_;
    bool holdNext_{false};
};

}  // namespace creator::app::test
