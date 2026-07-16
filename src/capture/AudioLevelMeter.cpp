#include "capture/AudioLevelMeter.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace creator::capture {
namespace {

constexpr double kDbfsFloor = -96.0;

double toDbfs(double linear) noexcept {
    if (linear <= 0.0) {
        return kDbfsFloor;
    }
    return std::max(kDbfsFloor, 20.0 * std::log10(linear));
}

creator::core::AppError malformed(std::string message) {
    return {creator::core::ErrorCode::InvalidArgument, std::move(message)};
}

}  // namespace

creator::core::Result<AudioLevel> AudioLevelMeter::measure(
    const creator::media::AudioBlock& block) {
    if (block.sampleRate == 0 || block.channels == 0 || block.frameCount == 0 ||
        !block.samples) {
        return malformed("audio block requires samples, rate, channels, and frames");
    }

    const auto sampleCount64 = static_cast<std::uint64_t>(block.channels) *
                               static_cast<std::uint64_t>(block.frameCount);
    if (sampleCount64 > std::numeric_limits<std::size_t>::max()) {
        return malformed("audio sample count exceeds addressable memory");
    }

    double peak = 0.0;
    double sumSquares = 0.0;
    const auto sampleCount = static_cast<std::size_t>(sampleCount64);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double sample = static_cast<double>(block.samples[index]);
        if (!std::isfinite(sample)) {
            return malformed("audio block contains a non-finite sample");
        }
        peak = std::max(peak, std::abs(sample));
        sumSquares += sample * sample;
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
    return AudioLevel{peak, rms, toDbfs(peak), toDbfs(rms)};
}

}  // namespace creator::capture
