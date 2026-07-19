#include "cut_suggest/CutSuggestion.h"

#include "core/AppError.h"

#include <cmath>
#include <utility>

namespace creator::cut_suggest {

using core::AppError;
using core::ErrorCode;
using core::Result;

Result<CutSuggestion> CutSuggestion::create(domain::TimeRange span,
                                            CutReason reason, double score,
                                            std::optional<std::string> label) {
    if (!std::isfinite(score)) {
        return AppError{ErrorCode::InvalidArgument,
                        "cut suggestion score must be finite"};
    }
    if (score < 0.0 || score > 1.0) {
        return AppError{ErrorCode::InvalidArgument,
                        "cut suggestion score must be within [0, 1]"};
    }
    if (label.has_value() && (label->empty() || !domain::isValidUtf8(*label))) {
        return AppError{ErrorCode::InvalidArgument,
                        "cut suggestion label must be non-empty valid UTF-8"};
    }
    return CutSuggestion{span, reason, score, std::move(label)};
}

}  // namespace creator::cut_suggest
