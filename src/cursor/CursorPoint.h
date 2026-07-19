#pragma once

#include "core/Result.h"

#include <cmath>

namespace creator::cursor {

/// A cursor position normalized to the capture source frame.
///
/// x and y are fractions of the source frame's width and height respectively,
/// each in the closed interval [0, 1]: (0,0) is the top-left corner and (1,1)
/// the bottom-right. Storing a fraction rather than a raw pixel keeps the value
/// independent of the recorded resolution, which is what later zoom math and
/// click highlights (R2-02/R2-03) need. The only way to construct one is
/// create(), so an out-of-range or non-finite point cannot exist.
class CursorPoint final {
public:
    /// Fails with InvalidArgument unless both coordinates are finite and lie in
    /// [0, 1]. Callers that start from raw pixels go through CursorNormalizer,
    /// which clamps before it reaches here; this factory is the last line of
    /// defense, not the clamping stage.
    [[nodiscard]] static core::Result<CursorPoint> create(double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "cursor point coordinates must be finite"};
        }
        if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "cursor point coordinates must be within [0, 1]"};
        }
        return CursorPoint{x, y};
    }

    [[nodiscard]] double x() const noexcept { return x_; }
    [[nodiscard]] double y() const noexcept { return y_; }

    friend bool operator==(const CursorPoint&, const CursorPoint&) = default;

private:
    CursorPoint(double x, double y) noexcept : x_(x), y_(y) {}

    double x_{};
    double y_{};
};

}  // namespace creator::cursor
