#include "audio_dsp/AudioLoudnessSerializer.h"

#include "core/AppError.h"

#include <cmath>

namespace creator::audio_dsp {
namespace {

using core::AppError;
using core::ErrorCode;

/// A finite double serialises as a JSON number; anything non-finite
/// (kNoMeasurement / ±inf / NaN) becomes JSON null so we never emit a value the
/// schema (and JSON itself) cannot represent.
nlohmann::json measurementToJson(double value) {
    if (std::isfinite(value)) {
        return nlohmann::json(value);
    }
    return nlohmann::json(nullptr);
}

}  // namespace

core::Result<nlohmann::json> AudioLoudnessSerializer::toJson(
    const AudioLoudnessSample& sample) {
    const auto tNs = sample.tNs.time_since_epoch().count();
    if (tNs < 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "audio.loudness sample has a negative timestamp; the "
                        "telemetry timebase requires tNs >= 0"};
    }

    nlohmann::json event;
    event["type"] = "audio.loudness";
    event["tNs"] = tNs;
    event["integratedLufs"] = measurementToJson(sample.integratedLufs);
    event["momentaryLufs"] = measurementToJson(sample.momentaryLufs);
    event["shortTermLufs"] = measurementToJson(sample.shortTermLufs);
    event["truePeakDbtp"] = measurementToJson(sample.truePeakDbtp);
    if (sample.sourceId.has_value() && !sample.sourceId->empty()) {
        event["sourceId"] = *sample.sourceId;
    }
    return event;
}

}  // namespace creator::audio_dsp
