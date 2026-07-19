#pragma once

#include "core/Result.h"
#include "transcription/Transcript.h"

#include <nlohmann/json_fwd.hpp>

namespace creator::transcription {

/// Serialization boundary between a Transcript and its on-disk JSON document.
///
/// Responsibility: convert a Transcript to a schema-versioned nlohmann::json
/// document and back, losslessly (round-trip equal). The produced document
/// conforms to schemas/transcript.schema.json. Deserialization re-validates
/// every value through the domain create() functions, so a hand-edited or
/// corrupt document (negative time, non-monotonic segments, non-finite
/// confidence, unknown schema version) is rejected with a core::AppError rather
/// than trusted. No exception crosses this boundary (CLAUDE.md 4).
class TranscriptSerializer final {
public:
    /// The schema version written into every document. Bumped only with a
    /// forward migration (CLAUDE.md 6).
    static constexpr int kSchemaVersion = 1;

    [[nodiscard]] static nlohmann::json toJson(const Transcript& transcript);

    [[nodiscard]] static core::Result<Transcript> fromJson(const nlohmann::json& document);
};

}  // namespace creator::transcription
