#pragma once

#include "avatar/ExpressionParameters.h"
#include "core/Timebase.h"

namespace creator::avatar {

/// One tracking engine's output for a single processed video frame.
///
/// Provider-neutral: whether the engine behind ITrackingProvider is MediaPipe,
/// OpenSeeFace or a deterministic fake, its output is reduced to this shape
/// before it crosses the port boundary (ARCHITECTURE.md 14).
///
/// `raw` is deliberately unadjusted per-frame output, not yet passed through a
/// calibration baseline (that is ExpressionNormalizer's job, added in a later
/// task) — keeping the two separate lets a provider be tested without also
/// pinning down calibration behaviour.
struct TrackingResult final {
    /// Project-timebase timestamp, taken from the source VideoFrame — never
    /// read from a wall clock (CLAUDE.md 9).
    core::TimestampNs timestamp{};
    ExpressionParameters raw{};
    /// Engine confidence in this result, normalized to [0, 1].
    ///
    /// Captured but not yet consumed in Stage A: `ExpressionNormalizer` only
    /// reads `faceFound`, and `avatar.motion` telemetry has no confidence
    /// slot. This field is reserved for Stage B, once a real provider exists
    /// to supply a meaningful value — a likely use is gating normalization on
    /// a low-confidence threshold (treated like `!faceFound`) and/or
    /// recording it in telemetry. Until then, treat it as a placeholder, not
    /// a live signal.
    float confidence{0.0F};
    /// Whether a face was found in the frame at all. When false, `raw` and
    /// `confidence` carry no meaningful signal — callers must not treat them
    /// as a stale-but-valid expression.
    bool faceFound{false};
};

}  // namespace creator::avatar
