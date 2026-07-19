#include "project_store/PersistentRenderJobLifecycle.h"

#include "core/AppError.h"
#include "project_store/RenderJobRecovery.h"

#include <cmath>
#include <optional>
#include <utility>

namespace creator::project_store {
namespace {

core::AppError stateError(std::string message) {
    return core::AppError{core::ErrorCode::InvalidState, std::move(message)};
}

bool terminal(edit_engine::RenderJobState state) noexcept {
    return state == edit_engine::RenderJobState::Completed ||
           state == edit_engine::RenderJobState::Failed ||
           state == edit_engine::RenderJobState::Cancelled;
}

}  // namespace

PersistentRenderJobLifecycle::PersistentRenderJobLifecycle(
    std::shared_ptr<IRenderJobStore> store)
    : store_(std::move(store)) {}

core::Result<void> PersistentRenderJobLifecycle::begin(
    const edit_engine::RenderRequest& request,
    const std::filesystem::path& partial, core::DurationNs totalDuration) {
    if (!store_) return stateError("render job store is unavailable");
    auto pending = edit_engine::RenderProgress::create(
        edit_engine::RenderJobState::Pending, 0.0, core::TimestampNs{},
        totalDuration);
    if (!pending.hasValue()) return pending.error();
    const auto now = core::Utc::now();
    RenderJobRecord record{
        .jobId = request.jobId(),
        .projectId = request.projectId(),
        .timelineId = request.snapshot().timeline.id(),
        .timelineRevision = request.snapshot().revision,
        .preset = request.preset(),
        .overwritePolicy = request.overwritePolicy(),
        .destination = request.destination(),
        .partial = partial,
        .progress = pending.value(),
        .diagnostics = {},
        .createdAt = now,
        .startedAt = std::nullopt,
        .updatedAt = now,
        .finishedAt = std::nullopt};
    auto begun = store_->begin(record);
    if (!begun.hasValue()) return begun;
    std::lock_guard lock(mutex_);
    auto [position, inserted] = sessions_.emplace(
        request.jobId().value(),
        Session{pending.value(), {}, now});
    if (!inserted) return stateError("render lifecycle was begun twice");
    return core::ok();
}

core::Result<void> PersistentRenderJobLifecycle::encoderSelected(
    const domain::RenderJobId& jobId,
    const edit_engine::RenderEncoderDiagnostics& diagnostics) {
    std::lock_guard lock(mutex_);
    const auto position = sessions_.find(jobId.value());
    if (position == sessions_.end()) {
        return stateError("render lifecycle was not begun");
    }
    auto& session = position->second;
    session.diagnostics.attemptedEncoder = diagnostics.attemptedEncoders;
    session.diagnostics.selectedEncoder = diagnostics.selectedEncoder;
    if (!diagnostics.fallbackReason.empty()) {
        session.diagnostics.fallbackReason = diagnostics.fallbackReason;
    }
    return persist(jobId, session, session.persistedProgress, false);
}

core::Result<void> PersistentRenderJobLifecycle::advance(
    const domain::RenderJobId& jobId,
    const edit_engine::RenderProgress& progress) {
    std::lock_guard lock(mutex_);
    const auto position = sessions_.find(jobId.value());
    if (position == sessions_.end()) {
        return stateError("render lifecycle was not begun");
    }
    auto& session = position->second;
    if (progress.state() == edit_engine::RenderJobState::Running &&
        session.persistedProgress.state() == edit_engine::RenderJobState::Running &&
        progress.fraction() < session.persistedProgress.fraction() + 0.01) {
        return core::ok();
    }
    return persist(jobId, session, progress, false);
}

core::Result<void> PersistentRenderJobLifecycle::preparePublication(
    const domain::RenderJobId& jobId,
    const std::filesystem::path& partial,
    const edit_engine::RenderProgress& progress) {
    if (progress.state() != edit_engine::RenderJobState::Publishing) {
        return stateError("publication evidence requires publishing state");
    }
    auto evidence = inspectRenderArtifact(partial);
    if (!evidence.hasValue()) return evidence.error();
    std::lock_guard lock(mutex_);
    const auto position = sessions_.find(jobId.value());
    if (position == sessions_.end()) {
        return stateError("render lifecycle was not begun");
    }
    auto& session = position->second;
    session.diagnostics.outputSha256 = evidence.value().sha256;
    session.diagnostics.destinationIdentity = evidence.value().identity;
    return persist(jobId, session, progress, false);
}

core::Result<void> PersistentRenderJobLifecycle::finish(
    const domain::RenderJobId& jobId, edit_engine::RenderJobState state,
    std::string diagnostic) {
    if (!terminal(state)) return stateError("render finish state is not terminal");
    std::lock_guard lock(mutex_);
    const auto position = sessions_.find(jobId.value());
    if (position == sessions_.end()) {
        return stateError("render lifecycle was not begun");
    }
    auto& session = position->second;
    if (!diagnostic.empty()) session.diagnostics.diagnostic = std::move(diagnostic);
    const bool completed = state == edit_engine::RenderJobState::Completed;
    auto progress = edit_engine::RenderProgress::create(
        state, completed ? 1.0 : session.persistedProgress.fraction(),
        completed
            ? core::TimestampNs{session.persistedProgress.totalDuration()}
            : session.persistedProgress.renderedThrough(),
        session.persistedProgress.totalDuration());
    if (!progress.hasValue()) return progress.error();
    auto persisted = persist(jobId, session, progress.value(), true);
    if (persisted.hasValue()) sessions_.erase(position);
    return persisted;
}

core::Result<void> PersistentRenderJobLifecycle::persist(
    const domain::RenderJobId& jobId, Session& session,
    const edit_engine::RenderProgress& progress, bool isTerminal) {
    const auto now = core::Utc::now();
    const auto started = progress.state() == edit_engine::RenderJobState::Pending
                             ? std::optional<core::Utc>{}
                             : std::optional<core::Utc>{session.startedAt};
    RenderJobUpdate update{.progress = progress,
                           .diagnostics = session.diagnostics,
                           .startedAt = started,
                           .updatedAt = now,
                           .finishedAt = isTerminal
                                             ? std::optional<core::Utc>{now}
                                             : std::optional<core::Utc>{}};
    auto advanced = store_->advance(jobId, update);
    if (advanced.hasValue()) session.persistedProgress = progress;
    return advanced;
}

}  // namespace creator::project_store
