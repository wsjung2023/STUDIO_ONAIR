#include "audio_dsp/AudioLoudnessNdjsonSink.h"

#include "audio_dsp/AudioLoudnessSerializer.h"
#include "core/AppError.h"

#include <ios>
#include <string>
#include <utility>

namespace creator::audio_dsp {

using core::AppError;
using core::ErrorCode;
using core::Result;

Result<AudioLoudnessNdjsonSink> AudioLoudnessNdjsonSink::open(
    const std::filesystem::path& target) {
    // Append + binary: binary keeps '\n' a single byte on every platform so the
    // file is byte-for-byte NDJSON; app positions every write at end-of-file so
    // an existing log is extended rather than truncated.
    std::ofstream stream(target, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open()) {
        return AppError{ErrorCode::IoFailure,
                        "could not open audio.loudness NDJSON sink at '" +
                            target.string() + "'"};
    }
    return AudioLoudnessNdjsonSink{target, std::move(stream)};
}

Result<void> AudioLoudnessNdjsonSink::append(const AudioLoudnessSample& sample) {
    // Serialize first: a rejected sample (e.g. negative tNs) must not touch the
    // file, so no partial or invalid line can ever be observed.
    auto json = AudioLoudnessSerializer::toJson(sample);
    if (!json.hasValue()) {
        return json.error();
    }

    // Compact, single-line dump; NDJSON is one object per line.
    const std::string line = json.value().dump();
    stream_ << line << '\n';
    stream_.flush();
    if (!stream_.good()) {
        return AppError{ErrorCode::IoFailure,
                        "failed to append audio.loudness line to '" +
                            path_.string() + "'"};
    }
    return core::ok();
}

}  // namespace creator::audio_dsp
