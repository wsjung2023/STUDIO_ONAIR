#include "core/Timebase.h"

namespace creator::core {

ProjectClock::time_point ProjectClock::now() noexcept {
    return time_point{std::chrono::duration_cast<duration>(
        std::chrono::steady_clock::now().time_since_epoch())};
}

Result<FrameRate> FrameRate::create(std::int64_t numerator, std::int64_t denominator) {
    if (numerator <= 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "frame rate numerator must be strictly positive"};
    }
    if (denominator <= 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "frame rate denominator must be strictly positive"};
    }
    return FrameRate{numerator, denominator};
}

TimestampNs frameToTimestamp(std::int64_t frameIndex, FrameRate rate) noexcept {
    // ns = frameIndex * (denominator / numerator) * 1e9, reordered so the
    // multiply happens before the divide and the result stays exact when it
    // can be.
    //
    // Rounds up rather than truncating: frameIndex's exact PTS is
    // frameIndex * denominator * 1e9 / numerator, which is not an integer for
    // most (rate, frameIndex) pairs (e.g. frame 1 at 60fps is
    // 16'666'666.67ns). Truncating down would land a fraction of a
    // nanosecond before frame N's interval - i.e. still inside frame N-1's -
    // so timestampToFrame would read the frame back as N-1. Rounding up keeps
    // the result inside [frameIndex's start, frameIndex's end), which is what
    // makes timestampToFrame(frameToTimestamp(f)) == f hold for every frame.
    //
    // Overflow headroom: the intermediate is frameIndex * denominator * 1e9.
    // At 59.94fps (denominator 1001) two hours is 431568 frames, giving
    // ~4.3e17 - two orders of magnitude below int64 max (~9.2e18). The
    // + (numerator - 1) added for rounding is at most a few hundred thousand,
    // negligible against that headroom.
    const std::int64_t scaledNs = frameIndex * rate.denominator() * 1'000'000'000LL;
    const std::int64_t ns = (scaledNs + rate.numerator() - 1) / rate.numerator();
    return TimestampNs{DurationNs{ns}};
}

std::int64_t timestampToFrame(TimestampNs timestamp, FrameRate rate) noexcept {
    const std::int64_t ns = timestamp.time_since_epoch().count();
    return (ns * rate.numerator()) / (rate.denominator() * 1'000'000'000LL);
}

}  // namespace creator::core
