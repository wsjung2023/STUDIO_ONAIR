#pragma once

#include "audio_dsp/AudioFormat.h"
#include "core/Result.h"

#include <vector>

namespace creator::audio_dsp {

class LoudnessMeter;

/// The measure-and-decide outcome of the FIRST pass of an offline, two-pass
/// export loudness normalization.
///
/// All loudness values are LUFS and true peak is dBTP. `measuredLufs` carries
/// LoudnessMeter::kNoMeasurement (-inf) when the program is silence or too short
/// to gate; in that case `shouldNormalize` is false and `gainDb` is 0 — the
/// exporter must leave the program untouched rather than boost noise
/// (CLAUDE.md §5: audio loss / no-op is never silent, it is reported here).
struct ExportLoudnessDecision {
    double measuredLufs;         ///< Integrated LUFS of the whole program.
    double truePeakDbtp;         ///< True peak of the whole program, dBTP.
    double targetLufs;           ///< Echoed target integrated loudness.
    double truePeakCeilingDbtp;  ///< Echoed output true-peak ceiling.
    double gainDb;               ///< Static gain to reach target (target - measured); 0 if no-op.
    bool shouldNormalize;        ///< False for silence / below the noise floor.
};

/// Pure, offline "measure the export program and decide the gain" helper.
///
/// Loudness normalization is inherently two-pass: the single static gain that
/// lands a program on target cannot be known until the WHOLE program's
/// integrated loudness has been measured. This analyzer is exactly that first
/// pass, factored out of LoudnessNormalizer so a streaming/render-time exporter
/// can measure the mixed program in one sweep and then apply the decided gain
/// while it renders (pass 2), instead of mutating an owning buffer up front.
///
/// It reuses the existing LoudnessMeter for the BS.1770/R128 measurement and
/// mirrors LoudnessNormalizer's near-silence guard (LoudnessNormalizer::
/// kNoiseFloorLufs), so the gain it decides is identical to the gain
/// LoudnessNormalizer would apply for the same program — the two stay in lock
/// step by construction. It never modifies audio; it only measures and decides.
///
/// Intended two-pass export call site (documented per the coordination note; the
/// wiring itself is done separately in the MLT export path):
///
///   1. PASS 1 — after the program is mixed to interleaved 48 kHz PCM, call
///      `analyze(program, format)` to obtain an ExportLoudnessDecision.
///   2. If `decision.shouldNormalize`, apply `decision.gainDb` (a GainProcessor)
///      followed by a true-peak LimiterProcessor at `truePeakCeilingDbtp` as the
///      program is written to the export consumer — i.e. exactly the pass-2 body
///      of LoudnessNormalizer::normalize, but streamed at render time.
///   3. If not, write the program through unchanged and surface the no-op
///      decision to the export telemetry/UI.
///
/// The meter's coefficients are 48 kHz-only, so `analyze` requires a 48 kHz
/// format and reports the meter's error otherwise. Qt-free, RAII, Result-based;
/// no exception crosses the API boundary.
class ExportLoudnessAnalyzer final {
public:
    /// Validated configuration, matching LoudnessNormalizer::Parameters so the
    /// two agree on target/ceiling.
    struct Parameters {
        double targetLufs = -14.0;          ///< Desired integrated loudness (<= 0).
        double truePeakCeilingDbtp = -1.0;  ///< Output true-peak ceiling, dBTP (<= 0).
    };

    /// The only way to obtain an ExportLoudnessAnalyzer. Rejects a non-finite or
    /// positive target loudness and a non-finite or positive ceiling with
    /// InvalidArgument (delegated to the same validation LoudnessNormalizer uses).
    [[nodiscard]] static core::Result<ExportLoudnessAnalyzer> create(
        const Parameters& params);

    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }

    /// Measure `interleaved` at `format` (must be 48 kHz) and decide the
    /// normalization gain. Non-finite input is rejected via Result (never spread
    /// through the measurement). Does NOT modify `interleaved`.
    [[nodiscard]] core::Result<ExportLoudnessDecision> analyze(
        const std::vector<float>& interleaved, const AudioFormat& format) const;

    /// Decide normalization from a meter that has already been populated in
    /// streaming blocks. This is the bounded-memory first pass used by export.
    [[nodiscard]] core::Result<ExportLoudnessDecision> decide(
        const LoudnessMeter& meter) const;

private:
    explicit ExportLoudnessAnalyzer(const Parameters& params) noexcept
        : params_(params) {}

    Parameters params_;
};

}  // namespace creator::audio_dsp
