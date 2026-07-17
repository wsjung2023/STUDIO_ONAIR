#include "avatar/PrimaryFaceSelector.h"

namespace creator::avatar {

std::optional<TrackingResult> selectPrimaryFace(std::span<const TrackingResult> candidates) {
    const TrackingResult* best = nullptr;
    for (const TrackingResult& candidate : candidates) {
        if (!candidate.faceFound) {
            continue;
        }
        // Strict '>' (not '>=') keeps the first equal-confidence candidate in
        // span order as the winner - the documented deterministic tie-break.
        if (best == nullptr || candidate.confidence > best->confidence) {
            best = &candidate;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

}  // namespace creator::avatar
