#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "core/Timebase.h"

#include <cmath>
#include <cstdint>
#include <span>

namespace creator::transcription {

/// A minimal, self-contained descriptor of decoded PCM audio handed to a
/// transcription provider.
///
/// Responsibility: carry a *non-owning* view of interleaved 32-bit float PCM
/// plus the format needed to interpret it (sample rate, channel count), so the
/// transcription module proves its pipeline without depending on cs_audio_dsp,
/// cs_capture, or any Qt/FFmpeg/MLT type (CLAUDE.md 3/5).
///
/// The span is non-owning: the caller owns the buffer and must keep it alive for
/// the whole transcribe() call. This type never mutates or copies the samples.
/// It deliberately holds no timestamp of its own: a provider maps the audio onto
/// the project timebase using its duration (CLAUDE.md 2.3), which is derived
/// from frame count and sample rate here.
class AudioInput final {
public:
    /// Fails with InvalidArgument unless sampleRateHz > 0, channelCount > 0, and
    /// the sample count is an exact multiple of channelCount (interleaved frames
    /// must be whole). An empty span is allowed here and is a valid "no audio"
    /// input; it is the provider's contract that decides whether zero frames is
    /// an error to transcribe.
    [[nodiscard]] static core::Result<AudioInput> create(
        std::span<const float> interleaved, std::int32_t sampleRateHz,
        std::int32_t channelCount) {
        if (sampleRateHz <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "audio sample rate must be positive"};
        }
        if (channelCount <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "audio channel count must be positive"};
        }
        if (interleaved.size() % static_cast<std::size_t>(channelCount) != 0) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "interleaved sample count must be a multiple of the channel count"};
        }
        return AudioInput{interleaved, sampleRateHz, channelCount};
    }

    [[nodiscard]] std::span<const float> samples() const noexcept { return samples_; }
    [[nodiscard]] std::int32_t sampleRateHz() const noexcept { return sampleRateHz_; }
    [[nodiscard]] std::int32_t channelCount() const noexcept { return channelCount_; }

    /// Number of interleaved frames (per-channel samples).
    [[nodiscard]] std::int64_t frameCount() const noexcept {
        return static_cast<std::int64_t>(samples_.size()) /
               static_cast<std::int64_t>(channelCount_);
    }

    /// Duration of the audio on the project timebase, computed exactly from the
    /// frame count and sample rate. Never reads a wall clock (CLAUDE.md 9).
    [[nodiscard]] core::DurationNs duration() const noexcept {
        constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
        const std::int64_t nanos =
            frameCount() * kNanosPerSecond / static_cast<std::int64_t>(sampleRateHz_);
        return core::DurationNs{nanos};
    }

    /// True unless the buffer contains a NaN or infinity. Providers may use this
    /// as a precondition guard; scanning is O(n) and is not done in create().
    [[nodiscard]] bool hasOnlyFiniteSamples() const noexcept {
        for (const float sample : samples_) {
            if (!std::isfinite(sample)) return false;
        }
        return true;
    }

private:
    AudioInput(std::span<const float> samples, std::int32_t sampleRateHz,
               std::int32_t channelCount) noexcept
        : samples_(samples), sampleRateHz_(sampleRateHz), channelCount_(channelCount) {}

    std::span<const float> samples_;
    std::int32_t sampleRateHz_;
    std::int32_t channelCount_;
};

}  // namespace creator::transcription
