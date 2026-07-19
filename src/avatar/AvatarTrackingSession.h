#pragma once

#include "avatar/AvatarMotionPipeline.h"
#include "avatar/ITrackingSource.h"
#include "core/Result.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace creator::avatar {

/// Owns one asynchronous tracking source and drives it through the common
/// motion pipeline. The session is intentionally worker-thread agnostic: the
/// caller controls polling cadence and supplies project timestamps.
class AvatarTrackingSession final {
public:
    AvatarTrackingSession(std::unique_ptr<ITrackingSource> source,
                          AvatarMotionPipeline& pipeline);

    AvatarTrackingSession(const AvatarTrackingSession&) = delete;
    AvatarTrackingSession& operator=(const AvatarTrackingSession&) = delete;
    AvatarTrackingSession(AvatarTrackingSession&&) = delete;
    AvatarTrackingSession& operator=(AvatarTrackingSession&&) = delete;
    ~AvatarTrackingSession();

    [[nodiscard]] core::Result<void> start(std::uint16_t port);
    [[nodiscard]] core::Result<std::optional<AvatarMotionSample>> poll(
        core::TimestampNs projectTime);
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    std::unique_ptr<ITrackingSource> source_;
    AvatarMotionPipeline* pipeline_;
};

}  // namespace creator::avatar
