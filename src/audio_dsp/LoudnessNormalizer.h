#pragma once

#include "audio_dsp/AudioFormat.h"
#include "core/Result.h"

#include <cstddef>
#include <vector>

namespace creator::audio_dsp {

/// Outcome of one loudness-normalization pass, for telemetry and UI reporting.
///
/// All loudness values are LUFS and true peak is dBTP. When the input is
/// silence or below the noise floor, `normalized` is false, `appliedGainDb` is
/// 0, and `measuredLufsBefore` carries LoudnessMeter::kNoMeasurement (or the
/// sub-floor measurement) — the signal is returned untouched rather than boosted
/// into noise.
struct NormalizationResult {
    double measuredLufsBefore;   ///< Integrated LUFS measured over the input.
    double appliedGainDb;        ///< Static gain applied (target - measured), dB.
    double achievedLufsAfter;    ///< Integrated LUFS re-measured after processing.
    double truePeakAfterDbtp;    ///< True peak re-measured after processing, dBTP.
    bool normalized;             ///< False when skipped (silence / below floor).
};

/// Offline, two-pass loudness standardizer (EBU R128 / streaming-platform style).
///
/// Loudness normalization is inherently two-pass: you must measure the whole
/// program's integrated loudness before you know the single static gain that
/// brings it to target, so this is deliberately an OFFLINE operation over an
/// owning interleaved buffer, NOT a streaming per-block IAudioProcessor node
/// (unlike the real-time gain/compressor/limiter, whose gain reacts per sample).
///
/// One pass measures integrated LUFS with a LoudnessMeter; the second applies
/// `target - measured` dB through a GainProcessor and then a true-peak
/// LimiterProcessor at the ceiling, guaranteeing the output never exceeds
/// `truePeakCeilingDbtp` (CLAUDE.md §5: true-peak safety is not silently
/// skipped). The limiter's look-ahead latency is compensated internally by
/// flushing and re-aligning, so the returned buffer keeps the input's length and
/// timing (no A/V drift). Finally it re-measures to report what was achieved.
///
/// Near-silence guard: if the integrated measurement is kNoMeasurement (silence
/// or a program too short to gate) or falls below `kNoiseFloorLufs`, the pass is
/// a documented no-op — boosting such input would only amplify noise.
///
/// The meter's coefficients are 48 kHz-only, so `normalize` requires a 48 kHz
/// format and reports the meter's error otherwise. Qt-free, RAII, no exception
/// crosses the API boundary.
class LoudnessNormalizer final {
public:
    /// Below this integrated loudness the input is treated as effectively
    /// silent and left untouched rather than boosted into noise. -60 LUFS is far
    /// below any real program level yet above the meter's -70 LKFS absolute gate.
    static constexpr double kNoiseFloorLufs = -60.0;

    /// Validated configuration.
    struct Parameters {
        double targetLufs = -14.0;          ///< Desired integrated loudness (<= 0).
        double truePeakCeilingDbtp = -1.0;  ///< Output true-peak ceiling, dBTP (<= 0).
    };

    /// The only way to obtain a LoudnessNormalizer. Rejects a non-finite or
    /// positive target loudness and a non-finite or positive ceiling with
    /// InvalidArgument.
    [[nodiscard]] static core::Result<LoudnessNormalizer> create(
        const Parameters& params);

    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }

    /// Normalize `interleaved` in place at `format` (must be 48 kHz, channel
    /// count 1..). Non-finite input is rejected via Result (never spread through
    /// the signal). Returns the measured/applied/achieved figures.
    [[nodiscard]] core::Result<NormalizationResult> normalize(
        std::vector<float>& interleaved, const AudioFormat& format) const;

private:
    explicit LoudnessNormalizer(const Parameters& params) noexcept
        : params_(params) {}

    Parameters params_;
};

}  // namespace creator::audio_dsp
