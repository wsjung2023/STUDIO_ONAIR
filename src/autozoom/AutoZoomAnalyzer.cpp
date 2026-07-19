#include "autozoom/AutoZoomAnalyzer.h"

#include "autozoom/ZoomRegion.h"
#include "domain/TimelineTypes.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace creator::autozoom {
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

// A maximal contiguous run of move samples the cursor kept spatially tight.
// Indices refer into the input move span; the running sum lets the centroid be
// read without a second pass.
struct Dwell final {
    std::size_t first{};
    std::size_t last{};
    double sumX{};
    double sumY{};
    std::size_t count{};
};

[[nodiscard]] double distanceTo(const CursorPoint& p, double cx, double cy) noexcept {
    const double dx = p.x() - cx;
    const double dy = p.y() - cy;
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

Result<std::vector<ZoomCandidate>> AutoZoomAnalyzer::analyze(
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

    // Empty (or click-only) input is not an error: a region is anchored on where
    // the cursor moved, so with no moves there is simply nothing to zoom.
    if (moves.empty()) {
        return std::vector<ZoomCandidate>{};
    }

    // 1. Sweep moves into dwells, splitting whenever a sample roams beyond
    //    focusRadius of the current dwell's running centroid.
    const double focusRadius = parameters_.focusRadius();
    std::vector<Dwell> dwells;
    Dwell current{0, 0, moves[0].point().x(), moves[0].point().y(), 1};
    for (std::size_t i = 1; i < moves.size(); ++i) {
        const CursorPoint& p = moves[i].point();
        const double cx = current.sumX / static_cast<double>(current.count);
        const double cy = current.sumY / static_cast<double>(current.count);
        if (distanceTo(p, cx, cy) <= focusRadius) {
            current.sumX += p.x();
            current.sumY += p.y();
            current.last = i;
            ++current.count;
        } else {
            dwells.push_back(current);
            current = Dwell{i, i, p.x(), p.y(), 1};
        }
    }
    dwells.push_back(current);

    // 2/3. Turn qualifying dwells into candidates.
    const double maxZoomFactor = parameters_.maxZoomFactor();
    const double clickWeight = parameters_.clickWeight();
    const std::int64_t minDwellNs = parameters_.minDwellDuration().count();

    std::vector<ZoomCandidate> prelim;
    for (const Dwell& dwell : dwells) {
        const TimestampNs firstT = moves[dwell.first].tNs();
        const TimestampNs lastT = moves[dwell.last].tNs();
        const DurationNs spanNs = lastT - firstT;
        if (spanNs.count() <= 0) {
            continue;  // a zero-duration dwell is not a zoom worth proposing
        }

        // Clicks landing inside the dwell's time window strengthen (or, for a
        // brief dwell, alone justify) the suggestion.
        std::size_t clickCount = 0;
        for (const CursorClickEvent& click : clicks) {
            if (click.tNs() >= firstT && click.tNs() <= lastT) {
                ++clickCount;
            }
        }

        const bool longEnough = spanNs.count() >= minDwellNs;
        const bool clickCluster = clickCount >= 2;
        if (!longEnough && !clickCluster) {
            continue;  // neither dwelled long enough nor a click cluster: roaming
        }

        const double centroidX = dwell.sumX / static_cast<double>(dwell.count);
        const double centroidY = dwell.sumY / static_cast<double>(dwell.count);

        // Tightness: how close the samples hugged the centroid, in [0, 1].
        double spread = 0.0;
        for (std::size_t i = dwell.first; i <= dwell.last; ++i) {
            spread = std::max(spread, distanceTo(moves[i].point(), centroidX, centroidY));
        }
        const double tightness = std::clamp(1.0 - spread / focusRadius, 0.0, 1.0);

        // Tighter dwells zoom in harder, capped at maxZoomFactor.
        const double factor = std::clamp(1.0 + tightness * (maxZoomFactor - 1.0), 1.0,
                                         maxZoomFactor);

        // Clamp the center so the derived viewport stays inside the frame; the
        // ZoomRegion factory is the final guard behind this.
        const double half = 0.5 / factor;
        const double clampedX = std::clamp(centroidX, half, 1.0 - half);
        const double clampedY = std::clamp(centroidY, half, 1.0 - half);

        auto centerResult = CursorPoint::create(clampedX, clampedY);
        if (!centerResult) {
            return centerResult.error();
        }
        auto regionResult = ZoomRegion::create(centerResult.value(), factor);
        if (!regionResult) {
            return regionResult.error();
        }

        // Score blends dwell length, tightness, and weighted clicks into [0, 1].
        const double dwellScore =
            std::clamp(static_cast<double>(spanNs.count()) /
                           static_cast<double>(2 * minDwellNs),
                       0.0, 1.0);
        const double clickScore =
            std::clamp(clickWeight * static_cast<double>(clickCount), 0.0, 1.0);
        const double tightnessScore = tightness;
        const double score = std::clamp(
            0.5 * dwellScore + 0.3 * tightnessScore + 0.2 * clickScore, 0.0, 1.0);

        auto spanResult = TimeRange::create(firstT, spanNs);
        if (!spanResult) {
            return spanResult.error();
        }
        auto candidateResult =
            ZoomCandidate::create(spanResult.value(), regionResult.value(), score);
        if (!candidateResult) {
            return candidateResult.error();
        }
        prelim.push_back(std::move(candidateResult).value());
    }

    // 4. Merge candidates whose spans sit closer than minGap so the eventual
    //    playback does not zoom out and immediately back in. Dwells are already
    //    time-ordered and non-overlapping, so merging preserves both invariants.
    const DurationNs minGap = parameters_.minGap();
    std::vector<ZoomCandidate> merged;
    for (const ZoomCandidate& candidate : prelim) {
        if (!merged.empty()) {
            const ZoomCandidate& back = merged.back();
            const DurationNs gap = candidate.span().start() - back.span().end();
            if (gap < minGap) {
                const TimestampNs start = back.span().start();
                const TimestampNs end = candidate.span().end();
                auto mergedSpan = TimeRange::create(start, end - start);
                if (!mergedSpan) {
                    return mergedSpan.error();
                }
                // Keep the stronger suggestion's region and the higher score.
                const ZoomRegion& region =
                    candidate.score() >= back.score() ? candidate.region() : back.region();
                const double score = std::max(candidate.score(), back.score());
                auto mergedCandidate =
                    ZoomCandidate::create(mergedSpan.value(), region, score);
                if (!mergedCandidate) {
                    return mergedCandidate.error();
                }
                merged.back() = std::move(mergedCandidate).value();
                continue;
            }
        }
        merged.push_back(candidate);
    }

    return merged;
}

}  // namespace creator::autozoom
