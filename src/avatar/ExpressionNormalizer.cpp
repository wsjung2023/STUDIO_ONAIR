#include "avatar/ExpressionNormalizer.h"

#include <utility>

namespace creator::avatar {

ExpressionNormalizer::ExpressionNormalizer(CalibrationProfile profile)
    : profile_(std::move(profile)) {}

ExpressionParameters ExpressionNormalizer::normalize(const TrackingResult& result) const {
    if (!result.faceFound) {
        return ExpressionParameters::neutral();
    }
    return profile_.apply(result.raw);
}

}  // namespace creator::avatar
