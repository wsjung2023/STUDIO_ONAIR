#include "app/android/AndroidMediaCodecSession.h"

#include "core/AppError.h"

#include <limits>

namespace creator::app::android {
namespace {

core::AppError invalidState(const char* message) {
    return {core::ErrorCode::InvalidState, message};
}

bool validFormat(const AndroidMediaCodecFormat& format) noexcept {
    if (format.kind == AndroidMediaCodecKind::Video) {
        return format.width > 0 && format.height > 0 &&
               format.sampleRate == 0 && format.channels == 0;
    }
    return format.width == 0 && format.height == 0 &&
           format.sampleRate > 0 && format.channels > 0;
}

}  // namespace

AndroidMediaCodecFormat AndroidMediaCodecFormat::video(
    std::uint32_t width, std::uint32_t height) noexcept {
    return {.kind = AndroidMediaCodecKind::Video,
            .width = width,
            .height = height};
}

AndroidMediaCodecFormat AndroidMediaCodecFormat::audio(
    std::uint32_t sampleRate, std::uint32_t channels) noexcept {
    return {.kind = AndroidMediaCodecKind::Audio,
            .sampleRate = sampleRate,
            .channels = channels};
}

core::Result<std::uint64_t> AndroidMediaCodecSession::begin(
    core::TimestampNs startTime) {
    if (active_) return invalidState("MediaCodec segment is already active");
    generation_ = generation_ == std::numeric_limits<std::uint64_t>::max()
                      ? 1
                      : generation_ + 1;
    startTime_ = startTime;
    lastTimestamp_.reset();
    format_.reset();
    active_ = true;
    return generation_;
}

core::Result<void> AndroidMediaCodecSession::accept(
    std::uint64_t generation, core::TimestampNs timestamp,
    AndroidMediaCodecFormat format) {
    if (!active_ || generation != generation_) {
        return invalidState("MediaCodec input belongs to an inactive segment");
    }
    if (!validFormat(format) || timestamp < startTime_ ||
        (lastTimestamp_ && timestamp < *lastTimestamp_)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MediaCodec input format or timestamp is invalid"};
    }
    if (format_ && *format_ != format) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MediaCodec input format changed within a segment"};
    }
    format_ = format;
    lastTimestamp_ = timestamp;
    return core::ok();
}

core::Result<void> AndroidMediaCodecSession::finish(
    std::uint64_t generation, core::TimestampNs endTime) {
    if (!active_ || generation != generation_) {
        return invalidState("MediaCodec segment is not active");
    }
    if (!lastTimestamp_) {
        return invalidState("MediaCodec segment has no input");
    }
    if (endTime < *lastTimestamp_) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MediaCodec segment ends before its final input"};
    }
    active_ = false;
    return core::ok();
}

void AndroidMediaCodecSession::abort(std::uint64_t generation) noexcept {
    if (active_ && generation == generation_) active_ = false;
}

}  // namespace creator::app::android
