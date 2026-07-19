#pragma once

#include "core/Timebase.h"

#include <cstdint>
#include <optional>
#include <variant>

namespace creator::cursor {

/// A raw, un-normalized cursor movement as reported by a capture backend.
///
/// Coordinates are physical pixels within the source frame; the frame's own
/// width and height travel with the sample so the normalizer can turn the pixel
/// into a fraction without any shared geometry state (the design doc's geometry
/// generation records exist to carry exactly this, per source, over time). The
/// timestamp is already mapped onto the project timebase by the capture layer.
struct RawCursorMoveSample final {
    core::TimestampNs tNs{};
    std::int64_t x{};
    std::int64_t y{};
    std::uint32_t sourceWidth{};
    std::uint32_t sourceHeight{};
};

/// A raw mouse-button transition as reported by a capture backend. The button
/// is carried as its small integer enum ordinal so this struct stays free of
/// any higher-level type; ICursorSource consumers map it to CursorButton.
struct RawCursorClickSample final {
    core::TimestampNs tNs{};
    std::int64_t x{};
    std::int64_t y{};
    std::uint32_t sourceWidth{};
    std::uint32_t sourceHeight{};
    std::uint8_t button{};  // 0 = left, 1 = right, 2 = middle
};

/// One raw sample pulled from a cursor source: either a move or a click.
using RawCursorSample = std::variant<RawCursorMoveSample, RawCursorClickSample>;

/// Port for a source-separated cursor stream.
///
/// A cursor stream is an independent capture source, exactly like screen, camera
/// and microphone (CLAUDE.md §2.3, §3): it produces raw cursor move/click
/// samples already stamped on the project timebase. This port is pull-based -
/// poll() drains the next buffered sample and returns nullopt when the stream is
/// momentarily empty or exhausted - which keeps the contract free of platform
/// callback types crossing the boundary. No method throws: a real backend
/// reports failure out-of-band on its own lifecycle port (mirroring
/// ICaptureSource), never by throwing across this interface (CLAUDE.md §4).
///
/// DEFERRED (do NOT build in R2-01 core): the REAL OS cursor hook. On Windows
/// that is the application-owned Raw Input adapter (RIDEV_INPUTSINK +
/// GetPhysicalCursorPos, per the design doc); on macOS an explicit
/// "unavailable" adapter until Input Monitoring is designed and physically
/// tested. Those are platform capture, like the real camera/mic backends, and
/// slot in behind this port later. The only implementation shipped now is the
/// deterministic FakeCursorSource used by tests.
class ICursorSource {
public:
    virtual ~ICursorSource() = default;

    ICursorSource(const ICursorSource&) = delete;
    ICursorSource& operator=(const ICursorSource&) = delete;
    ICursorSource(ICursorSource&&) = delete;
    ICursorSource& operator=(ICursorSource&&) = delete;

    /// Returns the next buffered raw sample, or nullopt when none is currently
    /// available. Never blocks and never throws.
    [[nodiscard]] virtual std::optional<RawCursorSample> poll() = 0;

protected:
    ICursorSource() = default;
};

}  // namespace creator::cursor
