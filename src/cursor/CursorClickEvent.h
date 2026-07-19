#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorPoint.h"

namespace creator::cursor {

/// A single source-separated mouse-button click sample on the project timebase.
///
/// Like CursorMoveEvent it carries a project-timebase timestamp and normalized
/// position, plus the button whose transition it records. A click cannot be
/// truthfully reconstructed after the fact, so this is a first-class durable
/// event rather than something inferred from movement. The schema
/// (schemas/event.schema.json, cursor.click) does not carry a sourceId on click
/// events, so neither does this value object.
///
/// Constructed only through create(); a click with a backwards timestamp cannot
/// be built.
class CursorClickEvent final {
public:
    /// Fails with InvalidArgument if the timestamp is negative on the project
    /// timebase.
    [[nodiscard]] static core::Result<CursorClickEvent> create(core::TimestampNs tNs,
                                                               CursorPoint point,
                                                               CursorButton button) {
        if (tNs.time_since_epoch().count() < 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "cursor click timestamp must not be negative"};
        }
        return CursorClickEvent{tNs, point, button};
    }

    [[nodiscard]] core::TimestampNs tNs() const noexcept { return tNs_; }
    [[nodiscard]] const CursorPoint& point() const noexcept { return point_; }
    [[nodiscard]] CursorButton button() const noexcept { return button_; }

private:
    CursorClickEvent(core::TimestampNs tNs, CursorPoint point, CursorButton button)
        : tNs_(tNs), point_(point), button_(button) {}

    core::TimestampNs tNs_;
    CursorPoint point_;
    CursorButton button_;
};

}  // namespace creator::cursor
