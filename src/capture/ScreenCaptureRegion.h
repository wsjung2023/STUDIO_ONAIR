#pragma once

#include "core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace creator::capture {

/// A rectangular sub-area of a monitor, expressed in monitor-relative pixels.
///
/// This is a Qt-free value object (CLAUDE.md 5: domain/capture must not carry Qt
/// or FFmpeg types). Construction goes through makeScreenCaptureRegion so an
/// empty rectangle can never exist, mirroring the "typed value object" rule.
struct ScreenCaptureRegion final {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};

    friend bool operator==(const ScreenCaptureRegion&, const ScreenCaptureRegion&) = default;
};

/// Validates that a requested rectangle has non-zero extent. Accepts signed
/// inputs so a UI can pass raw spin-box values and get a single, typed error for
/// negative or empty rectangles instead of silently wrapping to huge unsigned
/// values (CLAUDE.md 9: never hide a capture-setup failure).
[[nodiscard]] creator::core::Result<ScreenCaptureRegion> makeScreenCaptureRegion(
    std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height);

/// Confirms the region fits fully inside a monitor of the given pixel size.
/// Returns InvalidArgument when the rectangle spills past the monitor bounds.
[[nodiscard]] creator::core::Result<void> ensureRegionWithinBounds(
    const ScreenCaptureRegion& region, std::uint32_t monitorWidth,
    std::uint32_t monitorHeight);

/// Encodes a region onto an opaque capture-target id so it can ride the existing
/// IScreenCaptureSourceFactory::create(CaptureTargetId) boundary without leaking
/// geometry types across the port. Format: "<baseId>#region=x,y,w,h".
[[nodiscard]] std::string encodeRegionTargetId(const std::string& baseId,
                                               const ScreenCaptureRegion& region);

/// The base target id plus an optional region parsed back out of an encoded id.
/// A plain id (no "#region=" suffix) yields the id unchanged and no region.
struct ParsedRegionTargetId final {
    std::string baseId;
    std::optional<ScreenCaptureRegion> region;
};

[[nodiscard]] ParsedRegionTargetId parseRegionTargetId(const std::string& targetId);

/// A tightly packed (stride == width*4) BGRA8 buffer produced by cropBgra8.
struct CroppedBgra8 final {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width{0};
    std::uint32_t height{0};
};

/// Crops the sub-rectangle out of a tightly packed (stride == srcWidth*4) BGRA8
/// frame, returning a freshly packed region buffer. Rejects a null buffer or a
/// region that does not fit inside the source dimensions rather than reading out
/// of bounds. This is the geometry the D3D read-back path relies on and the one
/// the unit tests exercise against synthetic frames.
[[nodiscard]] creator::core::Result<CroppedBgra8> cropBgra8(
    const std::uint8_t* pixels, std::uint32_t srcWidth, std::uint32_t srcHeight,
    const ScreenCaptureRegion& region);

}  // namespace creator::capture
