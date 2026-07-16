#include "app/ProjectSegmentLifecycleSink.h"

#include "core/AppError.h"

#include <algorithm>
#include <utility>

namespace creator::app {

core::Result<std::shared_ptr<ProjectSegmentLifecycleContext>>
ProjectSegmentLifecycleContext::create(
    std::shared_ptr<project_store::IProjectPackageStore> store,
    std::filesystem::path packagePath, domain::RecordingSession session) {
    if (!store) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Segment persistence requires a project store"};
    }
    if (packagePath.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Segment persistence requires a project package path"};
    }
    if (session.state() != domain::SessionState::Recording) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Segment persistence requires a recording session"};
    }
    return std::shared_ptr<ProjectSegmentLifecycleContext>{
        new ProjectSegmentLifecycleContext{std::move(store), std::move(packagePath),
                                           std::move(session)}};
}

ProjectSegmentLifecycleContext::ProjectSegmentLifecycleContext(
    std::shared_ptr<project_store::IProjectPackageStore> store,
    std::filesystem::path packagePath, domain::RecordingSession session)
    : store_(std::move(store)),
      packagePath_(std::move(packagePath)),
      session_(std::move(session)) {}

core::Result<void> ProjectSegmentLifecycleContext::begin(
    const domain::SegmentInfo& segment) {
    std::lock_guard lock{mutex_};
    if (segment.status != domain::SegmentStatus::Writing) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "A new segment must enter persistence as WRITING"};
    }
    return store_->beginSegment(packagePath_, session_.id(), segment);
}

core::Result<void> ProjectSegmentLifecycleContext::ready(
    const domain::SegmentInfo& segment) {
    std::lock_guard lock{mutex_};
    if (segment.status != domain::SegmentStatus::Ready) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "A published segment must enter persistence as READY"};
    }

    const auto existing = std::find_if(
        session_.segments().begin(), session_.segments().end(),
        [&segment](const domain::SegmentInfo& value) {
            return value.sourceId == segment.sourceId && value.index == segment.index;
        });
    if (existing != session_.segments().end()) {
        if (*existing == segment) return core::ok();
        return core::AppError{core::ErrorCode::AlreadyExists,
                              "Segment index is already READY with different metadata"};
    }

    auto candidate = session_;
    if (auto added = candidate.addSegment(segment); !added.hasValue()) {
        return added.error();
    }
    if (auto stored = store_->markSegmentReady(packagePath_, session_.id(), segment);
        !stored.hasValue()) {
        return stored.error();
    }
    session_ = std::move(candidate);
    return core::ok();
}

core::Result<void> ProjectSegmentLifecycleContext::failed(
    const domain::SourceId& sourceId, std::uint64_t segmentIndex) {
    std::lock_guard lock{mutex_};
    return store_->markSegmentFailed(packagePath_, session_.id(), sourceId, segmentIndex);
}

domain::RecordingSession ProjectSegmentLifecycleContext::sessionSnapshot() const {
    std::lock_guard lock{mutex_};
    return session_;
}

core::Result<domain::RecordingSession> ProjectSegmentLifecycleContext::complete(
    core::TimestampNs stoppedAt, const core::Utc& finishedAt) {
    std::lock_guard lock{mutex_};
    auto candidate = session_;
    if (auto stopped = candidate.stop(stoppedAt); !stopped.hasValue()) {
        return stopped.error();
    }
    if (auto stored = store_->completeRecording(packagePath_, candidate, finishedAt);
        !stored.hasValue()) {
        return stored.error();
    }
    session_ = candidate;
    return candidate;
}

ProjectSegmentLifecycleSink::ProjectSegmentLifecycleSink(
    std::shared_ptr<ProjectSegmentLifecycleContext> context)
    : context_(std::move(context)) {}

core::Result<void> ProjectSegmentLifecycleSink::begin(
    const domain::SegmentInfo& segment) {
    if (!context_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Segment lifecycle sink has no project context"};
    }
    return context_->begin(segment);
}

core::Result<void> ProjectSegmentLifecycleSink::ready(
    const domain::SegmentInfo& segment) {
    if (!context_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Segment lifecycle sink has no project context"};
    }
    return context_->ready(segment);
}

core::Result<void> ProjectSegmentLifecycleSink::failed(
    const domain::SourceId& sourceId, std::uint64_t segmentIndex) {
    if (!context_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Segment lifecycle sink has no project context"};
    }
    return context_->failed(sourceId, segmentIndex);
}

}  // namespace creator::app
