#pragma once

#include "avatar/TrackingResult.h"
#include "core/Result.h"
#include "core/Timebase.h"

#include <cstdint>
#include <vector>

namespace creator::avatar {

/// Self-driven tracking source port for providers that produce asynchronous
/// results (for example OpenSeeFace over UDP) instead of consuming a video
/// frame through ITrackingProvider. The caller owns project-time assignment and
/// polls a bounded result batch on its worker thread.
class ITrackingSource {
public:
    virtual ~ITrackingSource() = default;
    ITrackingSource(const ITrackingSource&) = delete;
    ITrackingSource& operator=(const ITrackingSource&) = delete;
    ITrackingSource(ITrackingSource&&) = delete;
    ITrackingSource& operator=(ITrackingSource&&) = delete;

    [[nodiscard]] virtual core::Result<void> start(std::uint16_t port) = 0;
    [[nodiscard]] virtual core::Result<std::vector<TrackingResult>> poll(
        core::TimestampNs projectTime) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t boundPort() const noexcept = 0;

protected:
    ITrackingSource() = default;
};

}  // namespace creator::avatar
