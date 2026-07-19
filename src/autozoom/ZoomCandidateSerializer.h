#pragma once

#include "autozoom/ZoomCandidate.h"
#include "core/Result.h"

#include <nlohmann/json.hpp>

namespace creator::autozoom {

/// Serializes a ZoomCandidate to JSON for persistence and telemetry.
///
/// The document is schema-versioned (schemas/zoom_candidate.schema.json,
/// schemaVersion 1) so a stored suggestion can be read back by a later editor
/// that may have evolved the shape. Time is emitted as integer nanosecond
/// counts on the project timebase (never a float, never a unitless magic
/// number, CLAUDE.md §4); the region is emitted as center + factor, matching
/// the ZoomRegion value object exactly rather than re-deriving a rectangle.
///
/// This is a pure value->JSON mapping: no I/O, no Qt. Writing the bytes to disk
/// is the caller's (application-layer) job.
class ZoomCandidateSerializer final {
public:
    /// The schema version this serializer emits.
    static constexpr int kSchemaVersion = 1;

    /// Never fails for a validly-constructed candidate (its invariants already
    /// hold), but returns Result for symmetry with the rest of the codebase and
    /// so a future non-finite guard has a place to live.
    [[nodiscard]] static core::Result<nlohmann::json> toJson(const ZoomCandidate& candidate);
};

}  // namespace creator::autozoom
