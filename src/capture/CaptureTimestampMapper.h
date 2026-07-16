#pragma once

#include "core/Result.h"
#include "core/Timebase.h"

#include <cstdint>
#include <optional>

namespace creator::capture {

/// An exact native media timestamp before it is mapped to the project clock.
///
/// This mirrors the value/timescale semantics of CoreMedia's CMTime without
/// exposing an Apple type through the capture port.
struct NativeTimestamp final {
    std::int64_t value{0};
    std::int32_t timescale{0};

    friend bool operator==(const NativeTimestamp&, const NativeTimestamp&) = default;
};

/// Maps one capture stream's exact native clock onto the project timebase.
///
/// The first accepted native timestamp becomes projectAnchor. Subsequent
/// samples use checked integer rational conversion; no wall clock or floating
/// point enters the media timeline. One mapper belongs to one stream start.
class CaptureTimestampMapper final {
public:
    explicit CaptureTimestampMapper(creator::core::TimestampNs projectAnchor) noexcept;

    [[nodiscard]] creator::core::Result<creator::core::TimestampNs> map(
        NativeTimestamp timestamp);

    /// Starts a new stream epoch. The next valid sample maps exactly to the new
    /// project anchor and may use a different native timescale.
    void reset(creator::core::TimestampNs projectAnchor) noexcept;

private:
    creator::core::TimestampNs projectAnchor_;
    std::optional<NativeTimestamp> nativeAnchor_;
    std::optional<std::int64_t> lastNativeValue_;
};

}  // namespace creator::capture

