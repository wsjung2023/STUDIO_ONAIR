#include "cursor_emphasis/EmphasisPlanner.h"

#include "cursor_emphasis/ClickEmphasis.h"
#include "cursor_emphasis/CursorHideSpan.h"
#include "domain/TimelineTypes.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace creator::cursor_emphasis {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using cursor::CursorClickEvent;
using cursor::CursorMoveEvent;
using cursor::CursorPoint;
using domain::TimeRange;

[[nodiscard]] double distanceBetween(const CursorPoint& a, const CursorPoint& b) noexcept {
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return std::sqrt(dx * dx + dy * dy);
}

// Both event streams must already be ordered on the project timebase; an
// out-of-order stream is a caller/mapping bug, not a recoverable input.
template <typename Event>
[[nodiscard]] bool isNonDecreasing(std::span<const Event> events) noexcept {
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i].tNs() < events[i - 1].tNs()) {
            return false;
        }
    }
    return true;
}

}  // namespace

Result<EmphasisPlan> EmphasisPlanner::plan(
    std::span<const CursorMoveEvent> moves,
    std::span<const CursorClickEvent> clicks) const {
    if (!isNonDecreasing(moves)) {
        return AppError{ErrorCode::InvalidArgument,
                        "cursor move events must be sorted by timestamp"};
    }
    if (!isNonDecreasing(clicks)) {
        return AppError{ErrorCode::InvalidArgument,
                        "cursor click events must be sorted by timestamp"};
    }

    // 1. Click emphasis: one directive per click, at its own point and time.
    std::vector<ClickEmphasis> emphases;
    emphases.reserve(clicks.size());
    for (const CursorClickEvent& click : clicks) {
        auto emphasis = ClickEmphasis::create(
            click.point(), click.tNs(), parameters_.clickEmphasisDuration(), click.button(),
            parameters_.clickEmphasisStyle(), parameters_.emphasisRadius());
        if (!emphasis) {
            return emphasis.error();
        }
        emphases.push_back(std::move(emphasis).value());
    }

    // 2. Idle detection: sweep moves holding an anchor (the point the cursor last
    //    settled at). A sample within minMovementRadius of the anchor is jitter
    //    and keeps the run alive without resetting its start time; the first
    //    sample beyond the radius closes the run at the previous sample. A closed
    //    run whose extent >= idleThreshold becomes one CursorHideSpan{Idle}.
    const double minMovementRadius = parameters_.minMovementRadius();
    const DurationNs idleThreshold = parameters_.idleThreshold();
    std::vector<CursorHideSpan> hideSpans;

    // Emits a hide span for the closed run [runStart, runEnd] if it dwelled long
    // enough. runEnd is the index of the last still sample (the sample that moved
    // away is NOT part of the idle span).
    const auto closeRun = [&](std::size_t runStart, std::size_t runEnd) -> Result<void> {
        const TimestampNs start = moves[runStart].tNs();
        const DurationNs extent = moves[runEnd].tNs() - start;
        if (extent < idleThreshold) {
            return core::ok();  // too brief to be idle
        }
        auto range = TimeRange::create(start, extent);
        if (!range) {
            return range.error();
        }
        auto span = CursorHideSpan::create(range.value(), HideReason::Idle);
        if (!span) {
            return span.error();
        }
        hideSpans.push_back(std::move(span).value());
        return core::ok();
    };

    if (!moves.empty()) {
        std::size_t runStart = 0;
        for (std::size_t i = 1; i < moves.size(); ++i) {
            if (distanceBetween(moves[i].point(), moves[runStart].point()) <=
                minMovementRadius) {
                continue;  // jitter: still idle relative to the anchor
            }
            // Real move: close the run at the previous (last still) sample, then
            // seed the next anchor at this moved-to sample.
            if (auto result = closeRun(runStart, i - 1); !result) {
                return result.error();
            }
            runStart = i;
        }
        // Close the trailing run.
        if (auto result = closeRun(runStart, moves.size() - 1); !result) {
            return result.error();
        }
    }

    return EmphasisPlan::create(std::move(emphases), std::move(hideSpans));
}

}  // namespace creator::cursor_emphasis
