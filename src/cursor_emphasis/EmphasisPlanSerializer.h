#pragma once

#include "core/Result.h"
#include "cursor_emphasis/EmphasisPlan.h"

#include <nlohmann/json.hpp>

namespace creator::cursor_emphasis {

/// Serializes an EmphasisPlan to JSON for persistence and telemetry.
///
/// The document is schema-versioned (schemas/emphasis_plan.schema.json,
/// schemaVersion 1) so a stored plan can be read back by a later editor that may
/// have evolved the shape. Time is emitted as integer nanosecond counts on the
/// project timebase (never a float, never a unitless magic number, CLAUDE.md
/// §4); buttons, styles, and hide reasons are emitted as their canonical schema
/// tokens.
///
/// This is a pure value->JSON mapping: no I/O, no Qt. Writing the bytes to disk
/// is the caller's (application-layer) job.
class EmphasisPlanSerializer final {
public:
    /// The schema version this serializer emits.
    static constexpr int kSchemaVersion = 1;

    /// Never fails for a validly-constructed plan (its invariants already hold),
    /// but returns Result for symmetry with the rest of the codebase and so a
    /// future guard has a place to live.
    [[nodiscard]] static core::Result<nlohmann::json> toJson(const EmphasisPlan& plan);
};

}  // namespace creator::cursor_emphasis
