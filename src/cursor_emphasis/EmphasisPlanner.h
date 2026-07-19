#pragma once

#include "core/Result.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor_emphasis/EmphasisPlan.h"
#include "cursor_emphasis/EmphasisPlanParameters.h"

#include <span>

namespace creator::cursor_emphasis {

/// Turns recorded cursor telemetry into an editable emphasis plan. Pure,
/// deterministic, offline (CLAUDE.md §8): no clock, no RNG, no I/O, no
/// Qt/FFmpeg/MLT — the same events in always yield the same plan out.
///
/// Heuristic (documented so the output is explainable):
///   1. Click emphasis: every recorded click becomes exactly one ClickEmphasis,
///      centred on the click's own point, starting at the click's own timestamp,
///      lasting clickEmphasisDuration, with the configured style and radius and
///      the click's button. Clicks in, directives out, one-to-one and in order.
///   2. Idle detection: sweep the moves in time order holding an "anchor" — the
///      point where the cursor last settled. While each new sample stays within
///      minMovementRadius of that anchor it is treated as jitter and does NOT
///      reset the timer; the anchor and its settle time are kept. The first
///      sample that travels beyond minMovementRadius ends the idle run at the
///      previous sample. If the run lasted at least idleThreshold it becomes one
///      CursorHideSpan{Idle} covering [anchor settle time, last still sample];
///      then the moved-to sample seeds the next anchor. The trailing run is
///      closed the same way. Runs partition the samples, so the spans are always
///      ordered and strictly non-overlapping.
///
/// The DEFERRED pieces (do NOT live here): the click-highlight / cursor-hide
/// RENDER and compositing (editor + MLT compositor, Codex R1 territory), any
/// cursor replacement artwork, and live emphasis during capture. This class only
/// proposes directives; a human edits and the compositor renders.
class EmphasisPlanner final {
public:
    explicit EmphasisPlanner(EmphasisPlanParameters parameters) noexcept
        : parameters_(parameters) {}

    /// Builds an ordered, invariant-holding EmphasisPlan from ordered move and
    /// click streams.
    ///
    /// Both spans must be sorted non-decreasing by project-timebase timestamp;
    /// an out-of-order stream fails with InvalidArgument rather than producing a
    /// misleading plan. Empty input is NOT an error: it yields an empty plan
    /// (nothing recorded, nothing to emphasise). The event value objects already
    /// guarantee finite, in-[0,1] positions and non-negative timestamps by their
    /// own construction, so the only runtime input fault this can observe is
    /// ordering.
    [[nodiscard]] core::Result<EmphasisPlan> plan(
        std::span<const cursor::CursorMoveEvent> moves,
        std::span<const cursor::CursorClickEvent> clicks) const;

private:
    EmphasisPlanParameters parameters_;
};

}  // namespace creator::cursor_emphasis
