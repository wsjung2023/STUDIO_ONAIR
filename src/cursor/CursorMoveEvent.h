#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "cursor/CursorPoint.h"
#include "domain/Identifiers.h"

namespace creator::cursor {

/// A single source-separated cursor movement sample on the project timebase.
///
/// It carries the project-timebase timestamp (CLAUDE.md §2.3: every captured
/// datum is stamped on the common monotonic project clock), the normalized
/// position, and the id of the screen source the cursor belongs to. The move
/// event is the durable record that later click-highlight, cursor-replacement,
/// and auto-zoom stages consume; it is not baked only into the video.
///
/// Constructed only through create(), so a move event with a backwards
/// timestamp or an out-of-range point cannot be built.
class CursorMoveEvent final {
public:
    /// Fails with InvalidArgument if the timestamp is negative on the project
    /// timebase. Project time never runs before its own origin, so a negative
    /// value is a mapping bug, not a recoverable input.
    [[nodiscard]] static core::Result<CursorMoveEvent> create(core::TimestampNs tNs,
                                                              CursorPoint point,
                                                              domain::SourceId sourceId) {
        if (tNs.time_since_epoch().count() < 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "cursor move timestamp must not be negative"};
        }
        return CursorMoveEvent{tNs, point, std::move(sourceId)};
    }

    [[nodiscard]] core::TimestampNs tNs() const noexcept { return tNs_; }
    [[nodiscard]] const CursorPoint& point() const noexcept { return point_; }
    [[nodiscard]] const domain::SourceId& sourceId() const noexcept { return sourceId_; }

private:
    CursorMoveEvent(core::TimestampNs tNs, CursorPoint point, domain::SourceId sourceId)
        : tNs_(tNs), point_(point), sourceId_(std::move(sourceId)) {}

    core::TimestampNs tNs_;
    CursorPoint point_;
    domain::SourceId sourceId_;
};

}  // namespace creator::cursor
