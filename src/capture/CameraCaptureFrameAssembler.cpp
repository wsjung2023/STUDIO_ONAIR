#include "capture/CameraCaptureFrameAssembler.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

CameraCaptureFrameAssembler::CameraCaptureFrameAssembler()
    : CameraCaptureFrameAssembler([] { return core::ProjectClock::now(); }) {}

CameraCaptureFrameAssembler::CameraCaptureFrameAssembler(core::TimestampNs projectAnchor) noexcept
    : anchorProvider_([projectAnchor] { return projectAnchor; }) {}

CameraCaptureFrameAssembler::CameraCaptureFrameAssembler(
    ProjectAnchorProvider anchorProvider)
    : anchorProvider_(std::move(anchorProvider)) {}

core::Result<media::VideoFrame> CameraCaptureFrameAssembler::assemble(
    NativeCameraFrame frame) {
    if (frame.width == 0 || frame.height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "camera frame has invalid dimensions"};
    }
    if (frame.pixelFormat == media::PixelFormat::Unknown) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "camera frame has unknown pixel format"};
    }
    if (!frame.platformHandle) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "camera frame has no platform handle"};
    }
    if (frame.timestamp.timescale <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "camera frame timestamp timescale must be positive"};
    }

    if (!timestampMapper_) {
        timestampMapper_.emplace(anchorProvider_());
    }
    auto timestamp = timestampMapper_->map(frame.timestamp);
    if (!timestamp.hasValue()) {
        return timestamp.error();
    }

    return media::VideoFrame{
        .timestamp = timestamp.value(),
        .width = frame.width,
        .height = frame.height,
        .visibleRect = media::PixelRect{0, 0, frame.width, frame.height},
        .contentWidth = frame.width,
        .contentHeight = frame.height,
        .contentScale = 1.0,
        .pointPixelScale = 1.0,
        .pixelFormat = frame.pixelFormat,
        .colorSpace = media::ColorSpace::Rec709Sdr,
        .platformHandle = std::move(frame.platformHandle),
    };
}

}  // namespace creator::capture
