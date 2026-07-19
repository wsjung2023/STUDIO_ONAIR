#pragma once

#include "audio_dsp/LoudnessMeter.h"
#include "core/Timebase.h"

#include <optional>
#include <string>

namespace creator::audio_dsp {

/// Immutable snapshot of the loudness state at one instant on the project
/// timebase.
///
/// This is a pure value object (Qt-free, allocation-light): a project-timebase
/// timestamp plus the four EBU R128 / BS.1770 measurements a LoudnessMeter
/// exposes, and an optional identifier for the audio source/track the reading
/// belongs to. It carries no measurement logic of its own.
///
/// The four measurement doubles may legitimately hold
/// LoudnessMeter::kNoMeasurement (−infinity) when the meter has not yet seen
/// enough audio; serialisation is responsible for mapping any non-finite value
/// to JSON null. The timestamp must be >= 0 on the project timebase, which the
/// serializer/sink enforce at the boundary (the schema requires tNs >= 0).
struct AudioLoudnessSample final {
    /// Instant on the project timebase this reading describes (>= 0).
    core::TimestampNs tNs{};

    /// Gated integrated loudness (LUFS); kNoMeasurement until measurable.
    double integratedLufs = LoudnessMeter::kNoMeasurement;

    /// Momentary (400 ms) loudness (LUFS); kNoMeasurement until measurable.
    double momentaryLufs = LoudnessMeter::kNoMeasurement;

    /// Short-term (3 s) loudness (LUFS); kNoMeasurement until measurable.
    double shortTermLufs = LoudnessMeter::kNoMeasurement;

    /// Maximum true-peak level so far (dBTP); kNoMeasurement before any sample.
    double truePeakDbtp = LoudnessMeter::kNoMeasurement;

    /// Optional audio source/track this reading belongs to. Empty means "not
    /// attributed to a specific source" and is omitted from the JSON.
    std::optional<std::string> sourceId{};

    /// Snapshot the current measurements of `meter` at project time `tNs`.
    /// A convenience factory so callers do not have to copy the four queries by
    /// hand; it performs no validation (that happens at the serialise boundary).
    [[nodiscard]] static AudioLoudnessSample snapshot(
        const LoudnessMeter& meter, core::TimestampNs tNs,
        std::optional<std::string> sourceId = std::nullopt) {
        return AudioLoudnessSample{tNs,
                                   meter.integratedLufs(),
                                   meter.momentaryLufs(),
                                   meter.shortTermLufs(),
                                   meter.truePeakDbtp(),
                                   std::move(sourceId)};
    }
};

}  // namespace creator::audio_dsp
