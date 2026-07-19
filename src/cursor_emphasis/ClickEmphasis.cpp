#include "cursor_emphasis/ClickEmphasis.h"

#include <cmath>

namespace creator::cursor_emphasis {

core::Result<ClickEmphasis> ClickEmphasis::create(cursor::CursorPoint position,
                                                  core::TimestampNs startNs,
                                                  core::DurationNs duration,
                                                  cursor::CursorButton button,
                                                  EmphasisStyle style, double radius) {
    if (startNs.time_since_epoch().count() < 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "click emphasis start must not be negative"};
    }
    if (duration.count() <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "click emphasis duration must be positive"};
    }
    if (!std::isfinite(radius) || radius <= 0.0 || radius > 1.0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "click emphasis radius must be finite and within (0, 1]"};
    }
    return ClickEmphasis{position, startNs, duration, button, style, radius};
}

}  // namespace creator::cursor_emphasis
