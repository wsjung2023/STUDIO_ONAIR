#include "capture/ScreenCaptureFrameAssembler.h"

#include "core/AppError.h"

#include <cmath>
#include <utility>

namespace creator::capture {

ScreenCaptureFrameAssembler::ScreenCaptureFrameAssembler()
    : ScreenCaptureFrameAssembler([] { return core::ProjectClock::now(); }) {}

ScreenCaptureFrameAssembler::ScreenCaptureFrameAssembler(
    core::TimestampNs projectAnchor) noexcept
    : anchorProvider_([projectAnchor] { return projectAnchor; }) {}

ScreenCaptureFrameAssembler::ScreenCaptureFrameAssembler(
    ProjectAnchorProvider anchorProvider)
    : anchorProvider_(std::move(anchorProvider)) {}

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
    if (frame.visibleRect.width == 0 || frame.visibleRect.height == 0) {
        frame.visibleRect = media::PixelRect{.width = frame.width, .height = frame.height};
    }
    const auto visibleRight = static_cast<std::uint64_t>(frame.visibleRect.x) +
                              frame.visibleRect.width;
    const auto visibleBottom = static_cast<std::uint64_t>(frame.visibleRect.y) +
                               frame.visibleRect.height;
    if (visibleRight > frame.width || visibleBottom > frame.height ||
        !std::isfinite(frame.contentScale) || frame.contentScale <= 0.0 ||
        !std::isfinite(frame.pointPixelScale) || frame.pointPixelScale <= 0.0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "complete screen frame has invalid content geometry"};
    }
    if (frame.contentWidth == 0) frame.contentWidth = frame.visibleRect.width;
    if (frame.contentHeight == 0) frame.contentHeight = frame.visibleRect.height;

    if (!timestampMapper_) timestampMapper_.emplace(anchorProvider_());
    auto timestamp = timestampMapper_->map(frame.timestamp);
    if (!timestamp.hasValue()) return timestamp.error();
    return std::optional<media::VideoFrame>{media::VideoFrame{
        .timestamp = timestamp.value(),
        .width = frame.width,
        .height = frame.height,
        .visibleRect = frame.visibleRect,
        .contentWidth = frame.contentWidth,
        .contentHeight = frame.contentHeight,
        .contentScale = frame.contentScale,
        .pointPixelScale = frame.pointPixelScale,
        .pixelFormat = frame.pixelFormat,
        .colorSpace = media::ColorSpace::Rec709Sdr,
        .platformHandle = std::move(frame.platformHandle),
    }};
}

}  // namespace creator::capture
