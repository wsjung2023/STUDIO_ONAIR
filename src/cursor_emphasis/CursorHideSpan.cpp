#include "cursor_emphasis/CursorHideSpan.h"

namespace creator::cursor_emphasis {

core::Result<CursorHideSpan> CursorHideSpan::create(domain::TimeRange span,
                                                    HideReason reason) {
    switch (reason) {
        case HideReason::Idle:
        case HideReason::ExplicitRegion:
            return CursorHideSpan{span, reason};
    }
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "unknown cursor hide reason"};
}

}  // namespace creator::cursor_emphasis
