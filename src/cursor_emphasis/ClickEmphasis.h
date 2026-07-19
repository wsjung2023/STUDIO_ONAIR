#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorPoint.h"

#include <string_view>

namespace creator::cursor_emphasis {

/// How a click should be visually emphasised.
///
/// The set is deliberately small and describes intent, not pixels: Ripple is an
/// expanding ring animation radiating from the click; Highlight is a steady
/// halo held for the directive's duration. The actual artwork and animation are
/// DEFERRED to the editor + MLT compositor (Codex R1); this enum only records
/// which treatment a directive asks for.
enum class EmphasisStyle {
    Ripple,
    Highlight,
};

/// The canonical schema token for a style. These strings are the serialized
/// form (schemas/emphasis_plan.schema.json) and must not drift from it.
[[nodiscard]] inline std::string_view toString(EmphasisStyle style) noexcept {
    switch (style) {
        case EmphasisStyle::Ripple:
            return "ripple";
        case EmphasisStyle::Highlight:
            return "highlight";
    }
    return "ripple";
}

/// Parses a canonical style token. Fails with InvalidArgument for anything
/// outside the allowlist rather than guessing, so a malformed persisted plan is
/// rejected instead of silently coerced.
[[nodiscard]] inline core::Result<EmphasisStyle> emphasisStyleFromString(
    std::string_view text) {
    if (text == "ripple") {
        return EmphasisStyle::Ripple;
    }
    if (text == "highlight") {
        return EmphasisStyle::Highlight;
    }
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "unknown emphasis style '" + std::string{text} + "'"};
}

/// One editable click-emphasis directive: emphasise a click at a place and time.
///
/// This is the unit R2-02's planner produces for every recorded click. It
/// carries:
///   - position: the normalized CursorPoint the click landed at (the emphasis
///               centres here).
///   - startNs:  when the emphasis begins, on the project timebase (CLAUDE.md
///               §2.3) — the click's own timestamp.
///   - duration: how long the emphasis is shown (a positive DurationNs).
///   - button:   which mouse button the click was (surfaced so the editor can,
///               e.g., colour right-clicks differently).
///   - style:    Ripple or Highlight.
///   - radius:   the normalized emphasis radius in (0, 1], resolution- and
///               aspect-independent exactly like CursorPoint.
///
/// The actual highlight/ripple RENDER and compositing are DEFERRED (editor + MLT
/// compositor, Codex R1); this value object only describes the directive a human
/// edits and the compositor later draws.
///
/// Constructed only through create(), so a directive with a non-positive
/// duration or an out-of-range radius cannot exist.
class ClickEmphasis final {
public:
    /// Fails with InvalidArgument if the timestamp is negative on the project
    /// timebase, if the duration is not positive, or if the radius is not finite
    /// or falls outside (0, 1].
    [[nodiscard]] static core::Result<ClickEmphasis> create(cursor::CursorPoint position,
                                                            core::TimestampNs startNs,
                                                            core::DurationNs duration,
                                                            cursor::CursorButton button,
                                                            EmphasisStyle style,
                                                            double radius);

    [[nodiscard]] const cursor::CursorPoint& position() const noexcept { return position_; }
    [[nodiscard]] core::TimestampNs startNs() const noexcept { return startNs_; }
    [[nodiscard]] core::DurationNs duration() const noexcept { return duration_; }
    [[nodiscard]] core::TimestampNs endNs() const noexcept { return startNs_ + duration_; }
    [[nodiscard]] cursor::CursorButton button() const noexcept { return button_; }
    [[nodiscard]] EmphasisStyle style() const noexcept { return style_; }
    [[nodiscard]] double radius() const noexcept { return radius_; }

    friend bool operator==(const ClickEmphasis&, const ClickEmphasis&) = default;

private:
    ClickEmphasis(cursor::CursorPoint position, core::TimestampNs startNs,
                  core::DurationNs duration, cursor::CursorButton button,
                  EmphasisStyle style, double radius) noexcept
        : position_(position),
          startNs_(startNs),
          duration_(duration),
          button_(button),
          style_(style),
          radius_(radius) {}

    cursor::CursorPoint position_;
    core::TimestampNs startNs_;
    core::DurationNs duration_;
    cursor::CursorButton button_;
    EmphasisStyle style_;
    double radius_{};
};

}  // namespace creator::cursor_emphasis
