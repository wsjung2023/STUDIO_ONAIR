#pragma once

#include "avatar/AvatarMotionPipeline.h"
#include "avatar/ITrackingProvider.h"
#include "capture/LatestVideoFrameMailbox.h"
#include "core/Result.h"

#include <memory>
#include <optional>

namespace creator::app {

/// Pulls the newest camera preview frame into an in-process avatar provider.
///
/// Capture callbacks stay independent from model inference: the mailbox owns
/// the bounded handoff, while this coordinator is called by the application
/// timer/worker that owns the provider. Older preview frames are intentionally
/// skipped when inference is slower than capture, so tracking cannot build an
/// unbounded queue or invent timestamps.
class AvatarFrameTrackingCoordinator final {
public:
    AvatarFrameTrackingCoordinator(
        std::shared_ptr<capture::LatestVideoFrameMailbox> mailbox,
        avatar::ITrackingProvider& provider, avatar::AvatarMotionPipeline& pipeline);

    AvatarFrameTrackingCoordinator(const AvatarFrameTrackingCoordinator&) = delete;
    AvatarFrameTrackingCoordinator& operator=(const AvatarFrameTrackingCoordinator&) = delete;
    AvatarFrameTrackingCoordinator(AvatarFrameTrackingCoordinator&&) = delete;
    AvatarFrameTrackingCoordinator& operator=(AvatarFrameTrackingCoordinator&&) = delete;

    /// Consumes at most one latest frame. No frame is a successful empty poll;
    /// a terminal capture error is returned before any pending frame.
    [[nodiscard]] core::Result<std::optional<avatar::AvatarMotionSample>> poll();

private:
    std::shared_ptr<capture::LatestVideoFrameMailbox> mailbox_;
    avatar::ITrackingProvider* provider_;
    avatar::AvatarMotionPipeline* pipeline_;
};

}  // namespace creator::app
