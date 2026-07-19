#pragma once

#include "capture/IPullCaptureSource.h"

#include <functional>
#include <optional>
#include <string>

namespace creator::capture {

/// Pull adapter that turns timestamped avatar renders into a normal video
/// capture source. The renderer is injected so the capture/recording layer
/// stays independent of Inochi2D, Qt, and GPU handles.
class AvatarRenderCaptureSource final : public IPullCaptureSource {
public:
    using Render = std::function<core::Result<media::VideoFrame>(
        core::TimestampNs timestamp)>;

    AvatarRenderCaptureSource(domain::SourceId id, std::string displayName,
                              Render render);

    [[nodiscard]] domain::SourceId id() const override;
    [[nodiscard]] std::string displayName() const override;
    [[nodiscard]] core::Result<void> start(const CaptureConfig& config) override;
    [[nodiscard]] core::Result<void> stop() override;
    [[nodiscard]] CaptureStats stats() const noexcept override;
    [[nodiscard]] core::Result<media::VideoFrame> tick() override;

private:
    domain::SourceId id_;
    std::string displayName_;
    Render render_;
    CaptureConfig config_{};
    std::optional<core::FrameRate> frameRate_;
    std::int64_t nextFrameIndex_{0};
    CaptureStats stats_{};
};

}  // namespace creator::capture
