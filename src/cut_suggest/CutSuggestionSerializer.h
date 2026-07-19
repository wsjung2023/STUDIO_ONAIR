#pragma once

#include "core/Result.h"
#include "cut_suggest/CutSuggestion.h"

#include <nlohmann/json.hpp>

#include <span>

namespace creator::cut_suggest {

/// Serializes a CutSuggestion to JSON for persistence and telemetry.
///
/// The document is schema-versioned (schemas/cut_suggestion.schema.json,
/// schemaVersion 1) so a stored suggestion can be read back by a later editor
/// that may have evolved the shape. Time is emitted as integer nanosecond counts
/// on the project timebase (never a float, never a unitless magic number,
/// CLAUDE.md 4); the reason is emitted as its stable lowercase wire form; the
/// optional label is emitted only when present.
///
/// This is a pure value->JSON mapping: no I/O, no Qt. Writing the bytes to disk
/// is the caller's (application-layer) job.
class CutSuggestionSerializer final {
public:
    /// The schema version this serializer emits.
    static constexpr int kSchemaVersion = 1;

    /// Never fails for a validly-constructed suggestion (its invariants already
    /// hold), but returns Result for symmetry with the rest of the codebase.
    [[nodiscard]] static core::Result<nlohmann::json> toJson(
        const CutSuggestion& suggestion);

    /// Emits a JSON array of suggestion documents, preserving order.
    [[nodiscard]] static core::Result<nlohmann::json> toJsonArray(
        std::span<const CutSuggestion> suggestions);
};

}  // namespace creator::cut_suggest
