#pragma once

#include "core/Result.h"
#include "domain/TimelineTypes.h"

#include <string_view>

namespace creator::cursor_emphasis {

/// Why the cursor is hidden or replaced over a span.
///
/// Idle marks a span the planner derived from telemetry: the cursor sat still
/// long enough that showing it adds nothing, so it may be faded/hidden.
/// ExplicitRegion marks a span a human (or a later region tool) asked to hide or
/// replace, independent of movement. The planner only ever emits Idle; the enum
/// admits ExplicitRegion so the same value object carries editor-authored spans.
enum class HideReason {
    Idle,
    ExplicitRegion,
};

/// The canonical schema token for a reason. These strings are the serialized
/// form (schemas/emphasis_plan.schema.json) and must not drift from it.
[[nodiscard]] inline std::string_view toString(HideReason reason) noexcept {
    switch (reason) {
        case HideReason::Idle:
            return "idle";
        case HideReason::ExplicitRegion:
            return "explicit_region";
    }
    return "idle";
}

/// Parses a canonical reason token. Fails with InvalidArgument for anything
/// outside the allowlist rather than guessing, so a malformed persisted plan is
/// rejected instead of silently coerced.
[[nodiscard]] inline core::Result<HideReason> hideReasonFromString(std::string_view text) {
    if (text == "idle") {
        return HideReason::Idle;
    }
    if (text == "explicit_region") {
        return HideReason::ExplicitRegion;
    }
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "unknown hide reason '" + std::string{text} + "'"};
}

/// One editable span over which the cursor should be hidden or replaced.
///
/// It is a domain::TimeRange on the project timebase plus the reason the span
/// exists. The TimeRange value object already guarantees a non-negative start
/// and a strictly positive duration by construction, so a zero-length or
/// backwards hide span cannot be built.
///
/// The actual cursor-hide/replace RENDER (and any replacement artwork) is
/// DEFERRED to the editor + MLT compositor (Codex R1); this value object only
/// marks where.
///
/// Constructed only through create().
class CursorHideSpan final {
public:
    /// Fails with InvalidArgument if the reason is not a recognised enumerator.
    /// The span's positive-duration invariant is already enforced by TimeRange,
    /// so it cannot fail here.
    [[nodiscard]] static core::Result<CursorHideSpan> create(domain::TimeRange span,
                                                             HideReason reason);

    [[nodiscard]] const domain::TimeRange& span() const noexcept { return span_; }
    [[nodiscard]] HideReason reason() const noexcept { return reason_; }

    friend bool operator==(const CursorHideSpan&, const CursorHideSpan&) = default;

private:
    CursorHideSpan(domain::TimeRange span, HideReason reason) noexcept
        : span_(span), reason_(reason) {}

    domain::TimeRange span_;
    HideReason reason_;
};

}  // namespace creator::cursor_emphasis
