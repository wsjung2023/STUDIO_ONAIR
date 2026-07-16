#include "capture/AudioCaptureBlockAssembler.h"

#include "core/AppError.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace creator::capture {

AudioCaptureBlockAssembler::AudioCaptureBlockAssembler()
    : AudioCaptureBlockAssembler([] { return core::ProjectClock::now(); }) {}

AudioCaptureBlockAssembler::AudioCaptureBlockAssembler(core::TimestampNs projectAnchor) noexcept
    : anchorProvider_([projectAnchor] { return projectAnchor; }) {}

AudioCaptureBlockAssembler::AudioCaptureBlockAssembler(ProjectAnchorProvider anchorProvider)
    : anchorProvider_(std::move(anchorProvider)) {}

core::Result<media::AudioBlock> AudioCaptureBlockAssembler::assemble(
    NativeAudioBlock block) {
    if (block.sampleRate == 0 || block.channels == 0 || block.frameCount == 0 ||
        !block.samples) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "audio capture block has invalid format metadata"};
    }

    const auto expected64 = static_cast<std::uint64_t>(block.channels) *
                            static_cast<std::uint64_t>(block.frameCount);
    if (expected64 > std::numeric_limits<std::size_t>::max() ||
        block.sampleCount != static_cast<std::size_t>(expected64)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "audio capture block sample count does not match its format"};
    }
    for (std::size_t index = 0; index < block.sampleCount; ++index) {
        if (!std::isfinite(block.samples[index])) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "audio capture block contains a non-finite sample"};
        }
    }
    if (block.timestamp.timescale <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "audio capture timestamp timescale must be positive"};
    }

    if (!timestampMapper_) {
        timestampMapper_.emplace(anchorProvider_());
    }
    auto timestamp = timestampMapper_->map(block.timestamp);
    if (!timestamp.hasValue()) {
        return timestamp.error();
    }

    std::shared_ptr<const float[]> immutableSamples = std::move(block.samples);
    return media::AudioBlock{
        .timestamp = timestamp.value(),
        .sampleRate = block.sampleRate,
        .channels = block.channels,
        .frameCount = block.frameCount,
        .samples = std::move(immutableSamples),
    };
}

}  // namespace creator::capture
