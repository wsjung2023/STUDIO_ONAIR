#pragma once

#include "autozoom/ZoomRegion.h"
#include "core/Result.h"
#include "domain/TimelineTypes.h"

namespace creator::autozoom {

/// A single suggested auto-zoom: when to zoom, where to, and how confident.
///
/// This is the editable suggestion unit R2-03 produces. It carries:
///   - span:   a domain::TimeRange on the project timebase covering the whole
///             gesture (zoom-in -> hold -> zoom-out). The exact in/out easing is
///             DEFERRED to the editor + compositor; the candidate only owns the
///             *when*.
///   - region: the ZoomRegion the view should settle on.
///   - score:  a confidence in [0, 1]. Higher means the heuristic is more sure
///             the viewer wants to be zoomed here (longer dwell / tighter
///             cluster / more clicks). The editor surfaces this so a human can
///             accept, tweak, or drop the suggestion; nothing auto-applies.
///
/// Constructed only through create(), so a candidate with a score outside
/// [0, 1] cannot exist. The TimeRange and ZoomRegion are already-valid value
/// objects, so their own invariants (positive duration, in-frame viewport) hold
/// by construction.
class ZoomCandidate final {
public:
    /// Fails with InvalidArgument if the score is not finite or falls outside
    /// the closed interval [0, 1].
    [[nodiscard]] static core::Result<ZoomCandidate> create(domain::TimeRange span,
                                                            ZoomRegion region,
                                                            double score);

    [[nodiscard]] const domain::TimeRange& span() const noexcept { return span_; }
    [[nodiscard]] const ZoomRegion& region() const noexcept { return region_; }
    [[nodiscard]] double score() const noexcept { return score_; }

    friend bool operator==(const ZoomCandidate&, const ZoomCandidate&) = default;

private:
    ZoomCandidate(domain::TimeRange span, ZoomRegion region, double score) noexcept
        : span_(span), region_(region), score_(score) {}

    domain::TimeRange span_;
    ZoomRegion region_;
    double score_{};
};

}  // namespace creator::autozoom
