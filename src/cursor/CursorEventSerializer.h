#pragma once

#include "core/Result.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"

#include <nlohmann/json.hpp>

namespace creator::cursor {

/// Serializes cursor value objects into schema-valid telemetry JSON.
///
/// The objects it produces validate against schemas/event.schema.json:
/// cursor.move carries {tNs, type, x, y, sourceId}; cursor.click carries
/// {tNs, type, x, y, button}. Timestamps are emitted as integer nanoseconds on
/// the project timebase.
///
/// Both functions return Result even though a validated value object cannot
/// normally hold a bad field: it is defense in depth against a future caller
/// that constructs an event by another route, and it keeps the
/// serialize-before-write contract (a rejected event yields no bytes) honest at
/// the type level rather than by convention. A negative timestamp or a
/// non-finite coordinate fails with InvalidArgument.
class CursorEventSerializer final {
public:
    [[nodiscard]] static core::Result<nlohmann::json> toJson(const CursorMoveEvent& event);
    [[nodiscard]] static core::Result<nlohmann::json> toJson(const CursorClickEvent& event);
};

}  // namespace creator::cursor
