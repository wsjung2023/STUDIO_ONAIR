#include "ffmpeg_adapter/BgraFrameMappers.h"

#include "core/AppError.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace creator::ffmpeg_adapter {

CpuBgraFrameBuffer::CpuBgraFrameBuffer(std::uint32_t width, std::uint32_t height,
                                       std::size_t rowBytes,
                                       std::vector<std::uint8_t> bytes)
    : width_(width),
      height_(height),
      rowBytes_(rowBytes),
      bytes_(std::move(bytes)) {}

core::Result<std::shared_ptr<CpuBgraFrameBuffer>> CpuBgraFrameBuffer::create(
    std::uint32_t width, std::uint32_t height, std::size_t rowBytes) {
    if (width == 0 || height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CPU BGRA frame dimensions must be positive"};
    }
    const auto minimumRowBytes = static_cast<std::size_t>(width) * 4ULL;
    if (rowBytes == 0) rowBytes = minimumRowBytes;
    if (rowBytes < minimumRowBytes ||
        rowBytes > std::numeric_limits<std::size_t>::max() / height) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CPU BGRA frame stride or allocation size is invalid"};
    }
    try {
        std::vector<std::uint8_t> bytes(rowBytes * height);
        return std::shared_ptr<CpuBgraFrameBuffer>{new CpuBgraFrameBuffer{
            width, height, rowBytes, std::move(bytes)}};
    } catch (const std::bad_alloc&) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Could not allocate CPU BGRA frame storage"};
    }
}

core::Result<recorder::MappedVideoFrame> CpuBgraFrameMapper::map(
    const media::VideoFrame& frame) {
    if (frame.pixelFormat != media::PixelFormat::Bgra8 || !frame.platformHandle) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CPU mapper requires a BGRA frame buffer handle"};
    }
    auto buffer = std::static_pointer_cast<CpuBgraFrameBuffer>(frame.platformHandle);
    if (buffer->width() != frame.width || buffer->height() != frame.height) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CPU BGRA handle dimensions do not match the video frame"};
    }
    return recorder::MappedVideoFrame{
        .timestamp = frame.timestamp,
        .width = buffer->width(),
        .height = buffer->height(),
        .rowBytes = buffer->rowBytes(),
        .data = buffer->data(),
        .owner = std::move(buffer),
    };
}

}  // namespace creator::ffmpeg_adapter
