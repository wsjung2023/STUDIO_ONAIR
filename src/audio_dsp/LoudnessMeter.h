#pragma once

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/KWeightingFilter.h"
#include "core/Result.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace creator::audio_dsp {

/// Passive EBU R128 / ITU-R BS.1770-4 loudness observer.
///
/// Unlike an IAudioProcessor it never modifies audio — it only *measures*.
/// Samples are fed block by block through `addBlock`; the meter K-weights each
/// channel, accumulates mean-square energy over 100 ms sub-blocks, and from
/// those derives momentary (400 ms), short-term (3 s) and gated integrated
/// loudness (LUFS), plus an oversampled true-peak level (dBTP).
///
/// Channel-order assumption (documented, since the buffer carries no layout):
/// channels are interpreted as L, R, C, Ls, Rs in that index order, matching
/// the BS.1770 weighting G = {1.0, 1.0, 1.0, 1.41, 1.41}. Mono is a single
/// channel with G = 1.0. There is no LFE slot in this mapping; a 6th+ channel
/// is weighted 1.0. TODO(r2-06): thread an explicit channel layout through the
/// format so LFE can be excluded and surrounds identified unambiguously.
///
/// Coefficients are 48 kHz-only, so `create` requires that rate.
class LoudnessMeter final {
public:
    /// Sentinel returned by the loudness/true-peak queries when there is no
    /// valid measurement yet (e.g. a signal shorter than one 400 ms block, or
    /// every block gated out). It is -infinity so callers can test with
    /// std::isfinite and it orders below any real LUFS value.
    static constexpr double kNoMeasurement =
        -std::numeric_limits<double>::infinity();

    /// Validates the 48 kHz precondition and derives per-channel weights.
    [[nodiscard]] static core::Result<LoudnessMeter> create(
        const AudioFormat& format);

    /// Feed one block of interleaved PCM. The block's format must match the
    /// meter's (same rate and channel count). Non-finite samples are rejected
    /// with InvalidArgument so a NaN cannot corrupt the running measurement.
    [[nodiscard]] core::Result<void> addBlock(const AudioBuffer& buffer);

    /// Momentary loudness: last 400 ms sliding window, ungated. kNoMeasurement
    /// until at least 400 ms has been fed.
    [[nodiscard]] double momentaryLufs() const;

    /// Short-term loudness: last 3 s sliding window, ungated. kNoMeasurement
    /// until at least 3 s has been fed.
    [[nodiscard]] double shortTermLufs() const;

    /// Integrated loudness over all fed audio with the two-stage gating
    /// (absolute -70 LKFS, then relative -10 LU). kNoMeasurement if no block
    /// survives the gates.
    [[nodiscard]] double integratedLufs() const;

    /// Maximum true-peak level seen so far, in dBTP (0 dBTP = full scale),
    /// measured on a >=4x oversampled reconstruction. kNoMeasurement before any
    /// sample is fed.
    [[nodiscard]] double truePeakDbtp() const;

    /// Discard all accumulated state, returning the meter to its just-created
    /// condition.
    void reset();

private:
    LoudnessMeter(const AudioFormat& format, KWeightingFilter filter);

    // --- geometry -----------------------------------------------------------
    [[nodiscard]] std::size_t framesPerSubBlock() const noexcept {
        return framesPerSubBlock_;
    }
    [[nodiscard]] double loudnessFromMeanSquares(
        const std::vector<double>& perChannelMeanSquare) const;
    [[nodiscard]] double slidingWindowLoudness(std::size_t subBlockCount) const;
    void finishSubBlock();

    AudioFormat format_;
    KWeightingFilter filter_;
    std::vector<double> channelWeights_;  // G_i, per channel
    std::size_t framesPerSubBlock_;       // 100 ms hop in frames

    // Sum of squared K-weighted samples for the in-progress 100 ms sub-block,
    // per channel, plus how many frames it currently holds.
    std::vector<double> currentSumSq_;
    std::size_t currentSubBlockFrames_ = 0;

    // Completed 100 ms sub-blocks, each holding per-channel sum-of-squares.
    // Only the last 30 are retained (enough for the 3 s short-term window and
    // the 400 ms momentary/gating windows).
    std::deque<std::vector<double>> recentSubBlocks_;

    // One weighted power value (Sum_i G_i * z_i) per completed 400 ms gating
    // block (75% overlap: a new block every 100 ms). Retained for the whole
    // signal so integrated gating can be recomputed on demand.
    std::vector<double> gatingBlockPowers_;

    // --- true peak ----------------------------------------------------------
    // Per-channel history of the most recent raw input samples for the
    // oversampling FIR, newest at index 0.
    std::vector<std::vector<double>> truePeakHistory_;
    std::vector<double> oversampleTaps_;  // flattened polyphase prototype
    double maxTruePeak_ = 0.0;
    bool sawAnySample_ = false;
};

}  // namespace creator::audio_dsp
