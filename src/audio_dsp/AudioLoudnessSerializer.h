#pragma once

#include "audio_dsp/AudioLoudnessSample.h"
#include "core/Result.h"

#include <nlohmann/json.hpp>

namespace creator::audio_dsp {

/// Stateless translator turning an AudioLoudnessSample into a schema-valid
/// `audio.loudness` telemetry event (schemas/event.schema.json).
///
/// Responsibility boundary: this is the single place that decides the JSON
/// shape. It maps every non-finite measurement (including
/// LoudnessMeter::kNoMeasurement, which is −infinity and thus not representable
/// in JSON) to a JSON `null`, and it rejects a negative timestamp — the schema
/// requires tNs >= 0 — via Result rather than emitting an invalid object. It
/// never throws across its boundary.
class AudioLoudnessSerializer final {
public:
    /// Produce the `audio.loudness` JSON object for `sample`.
    /// Fails with ErrorCode::InvalidArgument when the sample's timestamp is
    /// negative on the project timebase (schema violation the serializer refuses
    /// to encode).
    [[nodiscard]] static core::Result<nlohmann::json> toJson(
        const AudioLoudnessSample& sample);
};

}  // namespace creator::audio_dsp
