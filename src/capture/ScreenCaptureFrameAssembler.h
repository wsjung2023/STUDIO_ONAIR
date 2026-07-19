#pragma once

#include "capture/CaptureTimestampMapper.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace creator::capture {

enum class NativeScreenFrameStatus { Complete, Idle, Blank, Started, Suspended, Stopped };

/// Platform-neutral facts extracted from one native screen sample.
struct NativeScreenFrame final {
    NativeScreenFrameStatus status{NativeScreenFrameStatus::Complete};
    NativeTimestamp timestamp;
    std::uint32_t width{0};
    std::uint32_t height{0};
    creator::media::PixelRect visibleRect{};
    std::uint32_t contentWidth{0};
    std::uint32_t contentHeight{0};
    double contentScale{1.0};
    double pointPixelScale{1.0};
    creator::media::PixelFormat pixelFormat{creator::media::PixelFormat::Unknown};
    std::shared_ptr<void> platformHandle;
};

/// Filters native frame states and constructs timestamp-correct neutral frames.
/// Incomplete status is a normal ignored output; malformed data labelled
/// complete is an adapter error and must not be silently presented.
class ScreenCaptureFrameAssembler final {
public:
    using ProjectAnchorProvider =
        std::function<creator::core::TimestampNs()>;

    ScreenCaptureFrameAssembler();
    explicit ScreenCaptureFrameAssembler(creator::core::TimestampNs projectAnchor) noexcept;
    explicit ScreenCaptureFrameAssembler(ProjectAnchorProvider anchorProvider);

    [[nodiscard]] creator::core::Result<std::optional<creator::media::VideoFrame>> assemble(
        NativeScreenFrame frame);

private:
    ProjectAnchorProvider anchorProvider_;
    std::optional<CaptureTimestampMapper> timestampMapper_;
};

}  // namespace creator::capture
