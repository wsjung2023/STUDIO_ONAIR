#include "cursor_emphasis/EmphasisPlan.h"

#include <cstddef>

namespace creator::cursor_emphasis {

core::Result<EmphasisPlan> EmphasisPlan::create(std::vector<ClickEmphasis> clicks,
                                                std::vector<CursorHideSpan> hideSpans) {
    for (std::size_t i = 1; i < clicks.size(); ++i) {
        if (clicks[i].startNs() < clicks[i - 1].startNs()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "click emphases must be ordered by start time"};
        }
    }
    for (std::size_t i = 1; i < hideSpans.size(); ++i) {
        // Strictly non-overlapping: the next span may begin no earlier than the
        // previous one ends. Touching at an instant (start == previous end) is
        // allowed; sharing any interior instant is not.
        if (hideSpans[i].span().start() < hideSpans[i - 1].span().end()) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "cursor hide spans must be ordered and non-overlapping"};
        }
    }
    return EmphasisPlan{std::move(clicks), std::move(hideSpans)};
}

}  // namespace creator::cursor_emphasis
