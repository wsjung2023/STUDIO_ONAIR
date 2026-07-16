#pragma once

#include "capture/CaptureTimestampMapper.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

#include <cstdint>
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
    creator::media::PixelFormat pixelFormat{creator::media::PixelFormat::Unknown};
    std::shared_ptr<void> platformHandle;
};

/// Filters native frame states and constructs timestamp-correct neutral frames.
/// Incomplete status is a normal ignored output; malformed data labelled
/// complete is an adapter error and must not be silently presented.
class ScreenCaptureFrameAssembler final {
public:
    explicit ScreenCaptureFrameAssembler(creator::core::TimestampNs projectAnchor) noexcept;

    [[nodiscard]] creator::core::Result<std::optional<creator::media::VideoFrame>> assemble(
        NativeScreenFrame frame);

private:
    CaptureTimestampMapper timestampMapper_;
};

}  // namespace creator::capture

