#include "avatar/ExpressionNormalizer.h"

#include <utility>

namespace creator::avatar {

ExpressionNormalizer::ExpressionNormalizer(CalibrationProfile profile, float minConfidence)
    : profile_(std::move(profile)), minConfidence_(minConfidence) {}

ExpressionParameters ExpressionNormalizer::normalize(const TrackingResult& result) const {
    if (!result.faceFound || result.confidence < minConfidence_) {
        return ExpressionParameters::neutral();
    }
    return profile_.apply(result.raw);
}

}  // namespace creator::avatar
