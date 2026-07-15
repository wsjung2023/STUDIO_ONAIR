#pragma once

#include "core/Result.h"

#include <chrono>
#include <cstdint>

namespace creator::core {

/// The project-wide monotonic timebase. Every media sample and every telemetry
/// event is mapped onto this clock (ARCHITECTURE.md 5.1):
///
///     project_time_ns = normalized_source_time + source_offset_ns
///
/// Wall clock time is only ever used for logs and user-facing display. A user
/// changing the system clock mid-recording must not affect A/V sync, which is
/// why this is steady and not system_clock.
struct ProjectClock final {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<ProjectClock>;
    static constexpr bool is_steady = true;

    [[nodiscard]] static time_point now() noexcept;
};

/// A point on the project timeline.
///
/// Distinct from DurationNs by construction: TimestampNs + TimestampNs does not
/// compile, and TimestampNs - TimestampNs yields a DurationNs. This is what
/// "단위 없는 정수 시간 금지" (CLAUDE.md 4) buys us.
using TimestampNs = ProjectClock::time_point;

/// A span of time on the project timeline.
using DurationNs = ProjectClock::duration;

using Nanoseconds = std::chrono::nanoseconds;

/// A frame rate as an exact rational.
///
/// Never store a frame rate as a double. 59.94 is exactly 60000/1001; rounding
/// it accumulates error across a long recording, and ARCHITECTURE.md 5.3 budgets
/// a total of 40ms of A/V drift over two hours.
class FrameRate final {
public:
    /// Fails with InvalidArgument unless both numerator and denominator are
    /// strictly positive.
    [[nodiscard]] static Result<FrameRate> create(std::int64_t numerator,
                                                  std::int64_t denominator);

    [[nodiscard]] std::int64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] std::int64_t denominator() const noexcept { return denominator_; }

    friend bool operator==(const FrameRate&, const FrameRate&) = default;

private:
    FrameRate(std::int64_t numerator, std::int64_t denominator)
        : numerator_(numerator), denominator_(denominator) {}

    std::int64_t numerator_{};
    std::int64_t denominator_{};
};

/// Presentation timestamp of a frame index on the project timeline.
[[nodiscard]] TimestampNs frameToTimestamp(std::int64_t frameIndex, FrameRate rate) noexcept;

/// Index of the frame being presented at a timestamp. Truncates toward zero, so
/// a timestamp inside frame N maps to N.
[[nodiscard]] std::int64_t timestampToFrame(TimestampNs timestamp, FrameRate rate) noexcept;

}  // namespace creator::core
