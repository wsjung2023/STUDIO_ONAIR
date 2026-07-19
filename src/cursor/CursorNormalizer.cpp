#include "cursor/CursorNormalizer.h"

namespace creator::cursor {
namespace {

[[nodiscard]] double clampUnit(double value) noexcept {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

}  // namespace

core::Result<CursorPoint> CursorNormalizer::normalize(std::int64_t x,
                                                      std::int64_t y,
                                                      std::uint32_t sourceWidth,
                                                      std::uint32_t sourceHeight) {
    if (sourceWidth == 0 || sourceHeight == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "cursor source dimensions must be non-zero"};
    }
    const double nx = clampUnit(static_cast<double>(x) / static_cast<double>(sourceWidth));
    const double ny = clampUnit(static_cast<double>(y) / static_cast<double>(sourceHeight));
    return CursorPoint::create(nx, ny);
}

}  // namespace creator::cursor
