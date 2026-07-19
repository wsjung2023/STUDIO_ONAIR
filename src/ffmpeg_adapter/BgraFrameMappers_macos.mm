#include "ffmpeg_adapter/BgraFrameMappers.h"

#include "core/AppError.h"

#include <CoreVideo/CoreVideo.h>

#include <memory>
#include <new>
#include <utility>

namespace creator::ffmpeg_adapter {
namespace {

class LockedPixelBuffer final {
public:
    LockedPixelBuffer(std::shared_ptr<void> owner, CVPixelBufferRef buffer)
        : owner_(std::move(owner)), buffer_(buffer) {}
    ~LockedPixelBuffer() {
        CVPixelBufferUnlockBaseAddress(buffer_, kCVPixelBufferLock_ReadOnly);
    }

private:
    std::shared_ptr<void> owner_;
    CVPixelBufferRef buffer_;
};

}  // namespace

core::Result<recorder::MappedVideoFrame> MacCvPixelBufferFrameMapper::map(
    const media::VideoFrame& frame) {
    if (frame.pixelFormat != media::PixelFormat::Bgra8 || !frame.platformHandle) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "macOS mapper requires a retained BGRA CVPixelBuffer"};
    }
    auto buffer = static_cast<CVPixelBufferRef>(frame.platformHandle.get());
    if (CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
        CVPixelBufferGetWidth(buffer) != frame.width ||
        CVPixelBufferGetHeight(buffer) != frame.height) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CVPixelBuffer format or dimensions do not match the frame"};
    }
    const auto locked = CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    if (locked != kCVReturnSuccess) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Could not lock CVPixelBuffer for FFmpeg mapping"};
    }
    std::shared_ptr<LockedPixelBuffer> owner;
    try {
        owner = std::make_shared<LockedPixelBuffer>(frame.platformHandle, buffer);
    } catch (const std::bad_alloc&) {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        return core::AppError{core::ErrorCode::IoFailure,
                              "Could not retain locked CVPixelBuffer mapping"};
    }
    const auto* baseAddress =
        static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    if (baseAddress == nullptr) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Locked CVPixelBuffer has no CPU base address"};
    }
    return recorder::MappedVideoFrame{
        .timestamp = frame.timestamp,
        .width = frame.width,
        .height = frame.height,
        .rowBytes = CVPixelBufferGetBytesPerRow(buffer),
        .data = baseAddress,
        .owner = std::move(owner),
    };
}

}  // namespace creator::ffmpeg_adapter
