#pragma once

#include "autozoom/AutoZoomParameters.h"
#include "autozoom/ZoomCandidate.h"
#include "core/Result.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"

#include <span>
#include <vector>

namespace creator::autozoom {

/// Turns recorded cursor telemetry into ordered, non-overlapping zoom
/// suggestions. Pure, deterministic, offline (CLAUDE.md §8): no clock, no RNG,
/// no I/O, no Qt/FFmpeg/MLT — the same events in always yield the same
/// candidates out.
///
/// Heuristic (documented so the output is explainable):
///   1. Sweep the moves in time order, growing a "dwell" while each new sample
///      stays within focusRadius of the dwell's running centroid. The first
///      sample that ROAMS beyond focusRadius closes the current dwell and seeds
///      the next one. A dwell is therefore a maximal run of samples the cursor
///      kept tight together in space and time.
///   2. A closed dwell becomes a candidate if it lasted at least
///      minDwellDuration, OR if it contains at least two clicks (a tight click
///      cluster is intent even when brief). Roaming, which never accumulates a
///      long tight run, produces no candidate.
///   3. The candidate's region centers on the dwell centroid (clamped so the
///      viewport stays in frame) with a factor scaled by how tight the dwell
///      was, capped at maxZoomFactor. Its score blends dwell length, tightness,
///      and weighted click count into [0, 1].
///   4. Candidates whose time ranges sit closer than minGap are merged, so the
///      result is always ordered and strictly non-overlapping.
///
/// The DEFERRED pieces (do NOT live here): the zoom-region editing UI, the
/// keyframed zoom-in/out animation, and the actual zoom RENDER/compositing
/// (editor + MLT compositor, Codex R1 territory), plus any live auto-zoom during
/// capture. This class only proposes; a human edits and the compositor renders.
class AutoZoomAnalyzer final {
public:
    explicit AutoZoomAnalyzer(AutoZoomParameters parameters) noexcept
        : parameters_(parameters) {}

    /// Analyzes ordered move and click streams and returns ordered,
    /// non-overlapping candidates.
    ///
    /// Both spans must be sorted non-decreasing by project-timebase timestamp;
    /// an out-of-order stream fails with InvalidArgument rather than producing a
    /// misleading suggestion. Empty input is NOT an error: it yields an empty
    /// candidate list (there is simply nothing to zoom). The event value objects
    /// already guarantee finite, in-[0,1] positions and non-negative timestamps
    /// by their own construction, so the only runtime input fault this can
    /// observe is ordering.
    [[nodiscard]] core::Result<std::vector<ZoomCandidate>> analyze(
        std::span<const cursor::CursorMoveEvent> moves,
        std::span<const cursor::CursorClickEvent> clicks) const;

private:
    AutoZoomParameters parameters_;
};

}  // namespace creator::autozoom
