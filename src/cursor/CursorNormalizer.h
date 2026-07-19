#pragma once

#include "core/Result.h"
#include "cursor/CursorPoint.h"

#include <cstdint>

namespace creator::cursor {

/// Turns a raw pixel cursor coordinate into a normalized CursorPoint.
///
/// This is the "coordinate" half of R2-01: it converts a physical (x, y) pixel
/// within a capture source of a given width and height into a fraction in
/// [0, 1] on each axis. It is a free-standing pure function with no state, no
/// clock, and no I/O, so it is trivially deterministic and testable.
///
/// Convention (documented so downstream math can rely on it): a coordinate is
/// divided by the source dimension, so pixel 0 maps to 0.0 and pixel == width
/// maps to 1.0, with the geometric center (width/2) mapping to 0.5.
class CursorNormalizer final {
public:
    /// Normalizes (x, y) against a source of sourceWidth x sourceHeight pixels.
    ///
    /// Fails with InvalidArgument if either source dimension is zero (a source
    /// with no area has no meaningful coordinate space and dividing by it is
    /// undefined). Coordinates that fall outside the frame - a cursor dragged
    /// past the edge, or a negative physical coordinate on a multi-monitor
    /// desktop - are CLAMPED to the [0, 1] range rather than rejected: the
    /// telemetry stays continuous and the point remains schema-valid, and the
    /// clamping is intentional here, not silent data loss.
    [[nodiscard]] static core::Result<CursorPoint> normalize(std::int64_t x,
                                                             std::int64_t y,
                                                             std::uint32_t sourceWidth,
                                                             std::uint32_t sourceHeight);
};

}  // namespace creator::cursor
