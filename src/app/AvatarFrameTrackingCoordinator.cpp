#include "app/AvatarFrameTrackingCoordinator.h"

#include "core/AppError.h"

#include <utility>

namespace creator::app {

AvatarFrameTrackingCoordinator::AvatarFrameTrackingCoordinator(
    std::shared_ptr<capture::LatestVideoFrameMailbox> mailbox,
    avatar::ITrackingProvider& provider, avatar::AvatarMotionPipeline& pipeline)
    : mailbox_(std::move(mailbox)), provider_(&provider), pipeline_(&pipeline) {}

core::Result<std::optional<avatar::AvatarMotionSample>>
AvatarFrameTrackingCoordinator::poll() {
    if (!mailbox_ || provider_ == nullptr || pipeline_ == nullptr) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar frame tracking coordinator is unavailable"};
    }
    if (const auto error = mailbox_->takeError(); error.has_value()) {
        return *error;
    }
    auto frame = mailbox_->takeLatest();
    if (!frame.has_value()) return std::optional<avatar::AvatarMotionSample>{};
    auto sample = pipeline_->processFrame(*provider_, *frame);
    if (!sample.hasValue()) return sample.error();
    return std::optional<avatar::AvatarMotionSample>{std::move(sample).value()};
}

}  // namespace creator::app
