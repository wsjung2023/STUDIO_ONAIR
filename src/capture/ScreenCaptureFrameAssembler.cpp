#include "capture/ScreenCaptureFrameAssembler.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

ScreenCaptureFrameAssembler::ScreenCaptureFrameAssembler(
    core::TimestampNs projectAnchor) noexcept
    : timestampMapper_(projectAnchor) {}

core::Result<std::optional<media::VideoFrame>> ScreenCaptureFrameAssembler::assemble(
    NativeScreenFrame frame) {
    if (frame.status != NativeScreenFrameStatus::Complete) {
        return std::optional<media::VideoFrame>{};
    }
    if (frame.width == 0 || frame.height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "complete screen frame has invalid dimensions"};
    }
    if (frame.pixelFormat == media::PixelFormat::Unknown) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "complete screen frame has unknown pixel format"};
    }
    if (!frame.platformHandle) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "complete screen frame has no platform handle"};
    }

    auto timestamp = timestampMapper_.map(frame.timestamp);
    if (!timestamp.hasValue()) return timestamp.error();
    return std::optional<media::VideoFrame>{media::VideoFrame{
        .timestamp = timestamp.value(),
        .width = frame.width,
        .height = frame.height,
        .pixelFormat = frame.pixelFormat,
        .colorSpace = media::ColorSpace::Rec709Sdr,
        .platformHandle = std::move(frame.platformHandle),
    }};
}

}  // namespace creator::capture

