#pragma once

#include "avatar/TrackingResult.h"

#include <optional>
#include <span>

namespace creator::avatar {

/// Picks the single face a single-avatar app should track from the N
/// candidates one OpenSeeFace datagram can carry
/// (`OpenSeeFaceParser::parseDatagram` returns `std::vector<TrackingResult>`
/// because the wire format is multi-face, but this app only ever drives one
/// avatar).
///
/// Policy: the highest-confidence `faceFound` candidate wins; a candidate
/// with `faceFound == false` carries no meaningful signal (see
/// TrackingResult's doc) and can never be selected regardless of its
/// confidence value. Ties (equal confidence) resolve to the first matching
/// candidate in span order - deterministic, not "closest to previous frame"
/// or any other stateful policy, since this function is stateless and pure.
///
/// Stage-B note: OpenSeeFace's datagram also carries per-face landmark
/// bounds, which a later refinement could use for a "largest" or "most
/// central" policy instead of confidence alone. Not built here (YAGNI) -
/// confidence-based selection is the simplest defensible policy for Stage A
/// and this function's signature does not preclude a smarter policy later.
///
/// Returns `std::nullopt` when `candidates` is empty or when none of them
/// have `faceFound == true`.
[[nodiscard]] std::optional<TrackingResult> selectPrimaryFace(
    std::span<const TrackingResult> candidates);

}  // namespace creator::avatar
