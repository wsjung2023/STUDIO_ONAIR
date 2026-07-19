#pragma once

#include "core/Result.h"
#include "cursor_emphasis/ClickEmphasis.h"
#include "cursor_emphasis/CursorHideSpan.h"

#include <vector>

namespace creator::cursor_emphasis {

/// The result of the emphasis heuristic: what to emphasise, and where to hide.
///
/// It owns two ordered lists produced from one recording:
///   - clicks:    one ClickEmphasis per recorded click, ordered non-decreasing
///                by start time.
///   - hideSpans: cursor hide/replace spans, ordered by start time and strictly
///                non-overlapping (no two spans share any instant).
///
/// Both invariants are enforced in create(), so a plan that a downstream editor
/// or compositor reads can rely on them without re-checking. An empty plan (no
/// clicks, no hide spans) is a valid, in-range result — a recording with no
/// clicks and no idle simply has nothing to emphasise.
///
/// Constructed only through create().
class EmphasisPlan final {
public:
    /// Fails with InvalidArgument if the clicks are not ordered non-decreasing
    /// by start time, or if the hide spans are not ordered and strictly
    /// non-overlapping. The ordering guard is what lets consumers treat the plan
    /// as a timeline rather than an unordered bag.
    [[nodiscard]] static core::Result<EmphasisPlan> create(
        std::vector<ClickEmphasis> clicks, std::vector<CursorHideSpan> hideSpans);

    [[nodiscard]] const std::vector<ClickEmphasis>& clicks() const noexcept {
        return clicks_;
    }
    [[nodiscard]] const std::vector<CursorHideSpan>& hideSpans() const noexcept {
        return hideSpans_;
    }

    friend bool operator==(const EmphasisPlan&, const EmphasisPlan&) = default;

private:
    EmphasisPlan(std::vector<ClickEmphasis> clicks, std::vector<CursorHideSpan> hideSpans)
        : clicks_(std::move(clicks)), hideSpans_(std::move(hideSpans)) {}

    std::vector<ClickEmphasis> clicks_;
    std::vector<CursorHideSpan> hideSpans_;
};

}  // namespace creator::cursor_emphasis
