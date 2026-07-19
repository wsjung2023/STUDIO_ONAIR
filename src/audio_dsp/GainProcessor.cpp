#include "audio_dsp/GainProcessor.h"

#include "core/AppError.h"

#include <cmath>

namespace creator::audio_dsp {

namespace {

/// Decibels → linear amplitude factor. −∞ dB maps to exactly 0 (silence);
/// NaN/+∞ dB map to a non-finite factor, which process() rejects.
[[nodiscard]] double dbToLinear(double db) noexcept {
    return std::pow(10.0, db / 20.0);
}

}  // namespace

GainProcessor::GainProcessor(double gainDb,
                             std::chrono::milliseconds rampDuration) noexcept
    : gainDb_(gainDb),
      targetLinear_(dbToLinear(gainDb)),
      currentLinear_(dbToLinear(gainDb)),
      rampDuration_(rampDuration) {}

void GainProcessor::setGainDb(double gainDb) noexcept {
    gainDb_ = gainDb;
    targetLinear_ = dbToLinear(gainDb);
    // currentLinear_ is intentionally left where it is: process() ramps from
    // the currently-applied factor toward this new target.
}

void GainProcessor::reset() noexcept {
    currentLinear_ = targetLinear_;
}

core::Result<void> GainProcessor::process(AudioBuffer& buffer) {
    if (buffer.empty()) {
        return core::ok();  // nothing to do; valid input
    }

    if (!std::isfinite(targetLinear_)) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "GainProcessor gain is not a finite linear factor (NaN/+Inf dB)"};
    }

    // Refuse to process buffers containing NaN/Inf rather than silently
    // spreading them across the block (CLAUDE.md §5: no silent audio damage).
    const std::span<float> samples = buffer.samples();
    for (const float s : samples) {
        if (!std::isfinite(s)) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "GainProcessor received a non-finite input sample"};
        }
    }

    const double rampFrames = std::max(
        1.0, static_cast<double>(buffer.format().sampleRateHz()) *
                 (static_cast<double>(rampDuration_.count()) / 1000.0));
    const double increment = (targetLinear_ - currentLinear_) / rampFrames;
    const bool ascending = targetLinear_ >= currentLinear_;

    const std::uint32_t channels = buffer.channelCount();
    const std::size_t frames = buffer.frameCount();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float factor = static_cast<float>(currentLinear_);
        float* const base = buffer.data() + frame * channels;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            base[ch] *= factor;
        }

        if (increment != 0.0) {
            currentLinear_ += increment;
            if ((ascending && currentLinear_ > targetLinear_) ||
                (!ascending && currentLinear_ < targetLinear_)) {
                currentLinear_ = targetLinear_;
            }
        }
    }

    // Snap away tiny residue so a settled ramp reports the exact target.
    if (std::abs(currentLinear_ - targetLinear_) < 1e-12) {
        currentLinear_ = targetLinear_;
    }

    return core::ok();
}

}  // namespace creator::audio_dsp
