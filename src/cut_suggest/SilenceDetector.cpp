#include "cut_suggest/SilenceDetector.h"

#include "audio_dsp/AudioFormat.h"
#include "core/AppError.h"
#include "domain/TimelineTypes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace creator::cut_suggest {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::TimeRange;

// A run scored deeper than this many dB below the threshold earns full depth
// credit; anything shallower scales linearly. 30 dB comfortably separates true
// dead air from merely quiet speech.
constexpr double kDepthRangeDb = 30.0;

[[nodiscard]] TimestampNs frameToTimestamp(std::uint64_t frame,
                                           const audio_dsp::AudioFormat& format) noexcept {
    return TimestampNs{
        std::chrono::duration_cast<DurationNs>(format.durationForFrames(frame))};
}

}  // namespace

Result<std::vector<CutSuggestion>> SilenceDetector::detect(
    const audio_dsp::AudioBuffer& audio) const {
    std::vector<CutSuggestion> suggestions;
    if (audio.empty()) {
        return suggestions;  // nothing recorded: nothing to cut, not an error
    }

    // Reject non-finite input up front: an RMS over a NaN/inf is meaningless and
    // must never be silently treated as loud or silent (CLAUDE.md 5, 9).
    for (const float sample : audio.samples()) {
        if (!std::isfinite(sample)) {
            return AppError{ErrorCode::InvalidArgument,
                            "audio buffer contains a non-finite sample"};
        }
    }

    const audio_dsp::AudioFormat& format = audio.format();
    const std::uint64_t sampleRate = format.sampleRateHz();
    const std::uint32_t channels = audio.channelCount();
    const std::uint64_t totalFrames = audio.frameCount();

    // Window and minimum-run lengths in frames, derived from the real-time
    // parameters and this buffer's sample rate. Both are floored to at least one
    // frame so a very short window or a sub-frame minimum cannot divide by zero
    // or vanish.
    constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
    std::uint64_t windowFrames = static_cast<std::uint64_t>(
        parameters_.rmsWindow().count() * static_cast<std::int64_t>(sampleRate) /
        kNanosPerSecond);
    if (windowFrames == 0) windowFrames = 1;
    std::uint64_t minSilenceFrames = static_cast<std::uint64_t>(
        parameters_.minSilenceDuration().count() *
        static_cast<std::int64_t>(sampleRate) / kNanosPerSecond);
    if (minSilenceFrames == 0) minSilenceFrames = 1;

    const double thresholdLinear =
        std::pow(10.0, parameters_.silenceThresholdDbfs() / 20.0);

    // Sweep non-overlapping windows, accumulating maximal runs of silent ones.
    std::uint64_t runStartFrame = 0;
    std::uint64_t runEndFrame = 0;
    bool inRun = false;
    double runSumSquares = 0.0;
    std::uint64_t runSampleCount = 0;

    auto flushRun = [&]() -> Result<void> {
        if (!inRun) return core::ok();
        inRun = false;
        const std::uint64_t runFrames = runEndFrame - runStartFrame;
        if (runFrames < minSilenceFrames) return core::ok();

        const TimestampNs start = frameToTimestamp(runStartFrame, format);
        const TimestampNs end = frameToTimestamp(runEndFrame, format);
        const DurationNs duration = end - start;
        if (duration.count() <= 0) return core::ok();

        const double lengthScore = std::clamp(
            static_cast<double>(runFrames) /
                static_cast<double>(2 * minSilenceFrames),
            0.0, 1.0);
        const double runRms =
            runSampleCount == 0 ? 0.0
                                : std::sqrt(runSumSquares /
                                            static_cast<double>(runSampleCount));
        // Pure-zero silence has RMS 0 (-inf dBFS): full depth credit.
        const double belowDb =
            runRms <= 0.0 ? kDepthRangeDb
                          : parameters_.silenceThresholdDbfs() -
                                20.0 * std::log10(runRms);
        const double depthScore = std::clamp(belowDb / kDepthRangeDb, 0.0, 1.0);
        const double score =
            std::clamp(0.5 * lengthScore + 0.5 * depthScore, 0.0, 1.0);

        auto span = TimeRange::create(start, duration);
        if (!span) return span.error();
        auto suggestion = CutSuggestion::create(span.value(), CutReason::Silence, score);
        if (!suggestion) return suggestion.error();
        suggestions.push_back(std::move(suggestion).value());
        return core::ok();
    };

    for (std::uint64_t windowStart = 0; windowStart < totalFrames;
         windowStart += windowFrames) {
        const std::uint64_t windowEnd =
            std::min(windowStart + windowFrames, totalFrames);

        double sumSquares = 0.0;
        std::uint64_t sampleCount = 0;
        for (std::uint64_t frame = windowStart; frame < windowEnd; ++frame) {
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const double s = audio.sample(static_cast<std::size_t>(frame), ch);
                sumSquares += s * s;
                ++sampleCount;
            }
        }
        const double rms =
            sampleCount == 0 ? 0.0
                             : std::sqrt(sumSquares / static_cast<double>(sampleCount));
        const bool silent = rms < thresholdLinear;

        if (silent) {
            if (!inRun) {
                inRun = true;
                runStartFrame = windowStart;
                runSumSquares = 0.0;
                runSampleCount = 0;
            }
            runEndFrame = windowEnd;
            runSumSquares += sumSquares;
            runSampleCount += sampleCount;
        } else if (inRun) {
            auto flushed = flushRun();
            if (!flushed) return flushed.error();
        }
    }
    auto flushed = flushRun();
    if (!flushed) return flushed.error();

    return suggestions;
}

}  // namespace creator::cut_suggest
