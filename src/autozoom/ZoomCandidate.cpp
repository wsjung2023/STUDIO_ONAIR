#include "autozoom/ZoomCandidate.h"

#include <cmath>

namespace creator::autozoom {

core::Result<ZoomCandidate> ZoomCandidate::create(domain::TimeRange span,
                                                  ZoomRegion region,
                                                  double score) {
    if (!std::isfinite(score)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "zoom candidate score must be finite"};
    }
    if (score < 0.0 || score > 1.0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "zoom candidate score must be within [0, 1]"};
    }
    return ZoomCandidate{span, region, score};
}

}  // namespace creator::autozoom
