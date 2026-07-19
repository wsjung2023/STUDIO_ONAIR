#pragma once

#include "capture/CaptureTimestampMapper.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace creator::capture {

struct NativeCameraFrame final {
    NativeTimestamp timestamp;
    std::uint32_t width{0};
    std::uint32_t height{0};
    creator::media::PixelFormat pixelFormat{creator::media::PixelFormat::Unknown};
    std::shared_ptr<void> platformHandle;
};

/// Validates adapter-extracted camera metadata and lazily maps its native PTS.
class CameraCaptureFrameAssembler final {
public:
    using ProjectAnchorProvider = std::function<creator::core::TimestampNs()>;

    CameraCaptureFrameAssembler();
    explicit CameraCaptureFrameAssembler(creator::core::TimestampNs projectAnchor) noexcept;
    explicit CameraCaptureFrameAssembler(ProjectAnchorProvider anchorProvider);

    [[nodiscard]] creator::core::Result<creator::media::VideoFrame> assemble(
        NativeCameraFrame frame);

private:
    ProjectAnchorProvider anchorProvider_;
    std::optional<CaptureTimestampMapper> timestampMapper_;
};

}  // namespace creator::capture
