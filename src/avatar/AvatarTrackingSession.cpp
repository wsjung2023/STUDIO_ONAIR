#include "avatar/AvatarTrackingSession.h"

#include "core/AppError.h"

#include <utility>

namespace creator::avatar {

AvatarTrackingSession::AvatarTrackingSession(
    std::unique_ptr<ITrackingSource> source, AvatarMotionPipeline& pipeline)
    : source_(std::move(source)), pipeline_(&pipeline) {}

AvatarTrackingSession::~AvatarTrackingSession() { stop(); }

core::Result<void> AvatarTrackingSession::start(std::uint16_t port) {
    if (!source_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar tracking source is unavailable"};
    }
    if (source_->running()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar tracking session is already running"};
    }
    pipeline_->reset();
    return source_->start(port);
}

core::Result<std::optional<AvatarMotionSample>> AvatarTrackingSession::poll(
    core::TimestampNs projectTime) {
    if (!source_ || !source_->running()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar tracking session is not running"};
    }
    auto candidates = source_->poll(projectTime);
    if (!candidates.hasValue()) return candidates.error();
    if (candidates.value().empty()) return std::optional<AvatarMotionSample>{};
    auto sample = pipeline_->process(candidates.value());
    if (!sample.hasValue()) return sample.error();
    return std::optional<AvatarMotionSample>{std::move(sample).value()};
}

void AvatarTrackingSession::stop() noexcept {
    if (source_) source_->stop();
}

bool AvatarTrackingSession::running() const noexcept {
    return source_ != nullptr && source_->running();
}

}  // namespace creator::avatar
