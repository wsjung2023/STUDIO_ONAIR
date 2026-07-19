#pragma once

#include "core/Result.h"
#include "cursor/CursorPoint.h"

#include <cmath>

namespace creator::autozoom {

/// A normalized zoom target: a point to center on plus how far to zoom in.
///
/// Representation choice (center + zoomFactor, NOT a free {x,y,w,h} rect):
/// auto-zoom is driven by where the cursor *dwells*, which is naturally a point
/// (the dwell centroid) and a tightness (how far to zoom). Storing the center
/// and factor keeps that intent legible and keeps the value resolution- and
/// aspect-independent, exactly like cursor::CursorPoint. The axis-aligned
/// viewport rectangle the eventual compositor needs is a pure function of the
/// two (visibleX/Y/W/H below), so nothing is lost, and the "in-frame" invariant
/// is expressed once: the derived viewport must lie inside [0, 1] on both axes.
///
/// The viewport is treated as a fraction 1/factor of each normalized axis
/// (factor 2 shows the middle half of each axis). The eventual zoom RENDER and
/// keyframed animation are DEFERRED (editor + MLT compositor, Codex R1
/// territory); this value object only describes the still target a suggestion
/// points at.
///
/// Constructed only through create(), so a region whose viewport escapes the
/// frame, or whose factor would zoom *out*, cannot exist.
class ZoomRegion final {
public:
    /// Fails with InvalidArgument if the factor is not finite, is below 1.0 (a
    /// factor < 1 is a zoom-out, which auto-zoom never proposes), or if the
    /// viewport derived from center+factor would fall outside [0, 1] on either
    /// axis. Callers that start from a raw dwell centroid clamp the center to
    /// keep the viewport in frame before they reach here (AutoZoomAnalyzer);
    /// this factory is the last line of defense, not the clamping stage.
    [[nodiscard]] static core::Result<ZoomRegion> create(cursor::CursorPoint center,
                                                         double zoomFactor) {
        if (!std::isfinite(zoomFactor)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "zoom factor must be finite"};
        }
        if (zoomFactor < 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "zoom factor must be >= 1.0 (never a zoom-out)"};
        }
        const double half = 0.5 / zoomFactor;
        if (center.x() - half < -kEpsilon || center.x() + half > 1.0 + kEpsilon ||
            center.y() - half < -kEpsilon || center.y() + half > 1.0 + kEpsilon) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "zoom viewport must stay within the [0, 1] frame"};
        }
        return ZoomRegion{center, zoomFactor};
    }

    [[nodiscard]] const cursor::CursorPoint& center() const noexcept { return center_; }
    [[nodiscard]] double zoomFactor() const noexcept { return zoomFactor_; }

    /// The derived axis-aligned viewport rectangle in normalized frame
    /// coordinates. width == height == 1/factor; the rectangle is guaranteed by
    /// construction to lie within [0, 1].
    [[nodiscard]] double visibleWidth() const noexcept { return 1.0 / zoomFactor_; }
    [[nodiscard]] double visibleHeight() const noexcept { return 1.0 / zoomFactor_; }
    [[nodiscard]] double visibleX() const noexcept { return center_.x() - 0.5 / zoomFactor_; }
    [[nodiscard]] double visibleY() const noexcept { return center_.y() - 0.5 / zoomFactor_; }

    friend bool operator==(const ZoomRegion&, const ZoomRegion&) = default;

private:
    // Tolerance for the in-frame check: a center clamped upstream in double
    // arithmetic can land a hair outside the exact bound, and rejecting a
    // viewport that is off by 1e-9 would be a false negative, not a caught bug.
    static constexpr double kEpsilon = 1e-9;

    ZoomRegion(cursor::CursorPoint center, double zoomFactor) noexcept
        : center_(center), zoomFactor_(zoomFactor) {}

    cursor::CursorPoint center_;
    double zoomFactor_{1.0};
};

}  // namespace creator::autozoom
