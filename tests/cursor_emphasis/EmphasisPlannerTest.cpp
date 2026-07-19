#include "cursor_emphasis/EmphasisPlanner.h"
#include "cursor_emphasis/EmphasisPlanParameters.h"

#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorPoint.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorClickEvent;
using creator::cursor::CursorMoveEvent;
using creator::cursor::CursorPoint;
using creator::cursor_emphasis::EmphasisPlanner;
using creator::cursor_emphasis::EmphasisPlanParameters;
using creator::cursor_emphasis::EmphasisStyle;
using creator::cursor_emphasis::HideReason;
using creator::domain::SourceId;

std::int64_t msToNs(std::int64_t ms) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<DurationNs>(std::chrono::milliseconds{ms}).count());
}

TimestampNs at(std::int64_t ns) { return TimestampNs{DurationNs{ns}}; }

SourceId screen() { return SourceId::create("screen-1").value(); }

EmphasisPlanParameters defaults() { return EmphasisPlanParameters::create().value(); }

CursorMoveEvent moveAt(double x, double y, std::int64_t ms) {
    return CursorMoveEvent::create(at(msToNs(ms)), CursorPoint::create(x, y).value(), screen())
        .value();
}

CursorClickEvent clickAt(double x, double y, std::int64_t ms, CursorButton button) {
    return CursorClickEvent::create(at(msToNs(ms)), CursorPoint::create(x, y).value(), button)
        .value();
}

// ---- click emphasis ----------------------------------------------------

TEST(EmphasisPlannerTest, OneClickYieldsOneDirectiveAtPointAndTime) {
    const std::vector<CursorClickEvent> clicks{clickAt(0.3, 0.7, 1000, CursorButton::Left)};

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan({}, clicks);
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().clicks().size(), 1u);

    const auto& directive = result.value().clicks().front();
    EXPECT_EQ(directive.position(), CursorPoint::create(0.3, 0.7).value());
    EXPECT_EQ(directive.startNs(), at(msToNs(1000)));
    EXPECT_EQ(directive.duration(), EmphasisPlanParameters::kDefaultClickEmphasisDuration);
    EXPECT_EQ(directive.button(), CursorButton::Left);
    EXPECT_EQ(directive.style(), EmphasisStyle::Ripple);
    EXPECT_DOUBLE_EQ(directive.radius(), 0.06);
}

TEST(EmphasisPlannerTest, ThreeClicksYieldThreeOrderedDirectives) {
    const std::vector<CursorClickEvent> clicks{
        clickAt(0.2, 0.2, 100, CursorButton::Left),
        clickAt(0.5, 0.5, 500, CursorButton::Right),
        clickAt(0.8, 0.8, 900, CursorButton::Middle),
    };

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan({}, clicks);
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().clicks().size(), 3u);

    const auto& out = result.value().clicks();
    EXPECT_EQ(out[0].startNs(), at(msToNs(100)));
    EXPECT_EQ(out[1].startNs(), at(msToNs(500)));
    EXPECT_EQ(out[2].startNs(), at(msToNs(900)));
    EXPECT_EQ(out[1].button(), CursorButton::Right);
    EXPECT_EQ(out[2].button(), CursorButton::Middle);
    // Ordered non-decreasing by start.
    EXPECT_LE(out[0].startNs(), out[1].startNs());
    EXPECT_LE(out[1].startNs(), out[2].startNs());
}

// ---- idle detection ----------------------------------------------------

// The cursor sits at one spot for ~2.9s (> the 2s threshold) -> one idle span.
TEST(EmphasisPlannerTest, LongNoMovementGapYieldsOneIdleHideSpan) {
    std::vector<CursorMoveEvent> moves;
    for (std::int64_t ms = 0; ms <= 2900; ms += 100) {
        moves.push_back(moveAt(0.5, 0.5, ms));
    }

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan(moves, {});
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().hideSpans().size(), 1u);

    const auto& span = result.value().hideSpans().front();
    EXPECT_EQ(span.reason(), HideReason::Idle);
    EXPECT_EQ(span.span().start(), at(0));
    EXPECT_EQ(span.span().duration(), DurationNs{msToNs(2900)});
}

// Continuous movement (every step travels beyond min-movement radius) -> no idle.
TEST(EmphasisPlannerTest, ContinuousMovementYieldsNoHideSpans) {
    std::vector<CursorMoveEvent> moves;
    double x = 0.1;
    for (std::int64_t ms = 0; ms <= 3000; ms += 100) {
        moves.push_back(moveAt(x, 0.5, ms));
        x += 0.02;  // 0.02 > default minMovementRadius (0.01)
    }

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan(moves, {});
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().hideSpans().empty());
}

// Jitter below the min-movement radius does NOT reset the idle timer, so a long
// jittery dwell is still one idle span; a real move beyond the radius ends it.
TEST(EmphasisPlannerTest, JitterStaysIdleAndRealMoveEndsIt) {
    std::vector<CursorMoveEvent> moves;
    // 2.4s of sub-radius jitter around (0.5, 0.5): each sample <0.01 from anchor.
    for (std::int64_t ms = 0; ms <= 2400; ms += 100) {
        const double dx = ((ms / 100) % 2 == 0) ? 0.004 : -0.004;
        moves.push_back(moveAt(0.5 + dx, 0.5, ms));
    }
    // A genuine move well beyond the radius ends the idle run.
    moves.push_back(moveAt(0.8, 0.5, 2500));

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan(moves, {});
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().hideSpans().size(), 1u);

    const auto& span = result.value().hideSpans().front();
    EXPECT_EQ(span.reason(), HideReason::Idle);
    EXPECT_EQ(span.span().start(), at(0));
    // Covers up to the last still sample (2400ms), not the move at 2500ms.
    EXPECT_EQ(span.span().duration(), DurationNs{msToNs(2400)});
    EXPECT_LE(span.span().end(), at(msToNs(2500)));
}

// A brief pause under the threshold does not become an idle span.
TEST(EmphasisPlannerTest, ShortPauseBelowThresholdIsNotIdle) {
    std::vector<CursorMoveEvent> moves;
    for (std::int64_t ms = 0; ms <= 1000; ms += 100) {  // only 1s, < 2s threshold
        moves.push_back(moveAt(0.5, 0.5, ms));
    }

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan(moves, {});
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().hideSpans().empty());
}

// Two idle dwells separated by a move -> two ordered, non-overlapping spans.
TEST(EmphasisPlannerTest, TwoIdleDwellsYieldTwoOrderedNonOverlappingSpans) {
    std::vector<CursorMoveEvent> moves;
    for (std::int64_t ms = 0; ms <= 2500; ms += 100) {
        moves.push_back(moveAt(0.25, 0.25, ms));
    }
    // Move away, then settle elsewhere for another long idle.
    for (std::int64_t ms = 5000; ms <= 7600; ms += 100) {
        moves.push_back(moveAt(0.75, 0.75, ms));
    }

    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan(moves, {});
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().hideSpans().size(), 2u);

    const auto& spans = result.value().hideSpans();
    EXPECT_LE(spans[0].span().end(), spans[1].span().start());
}

// ---- error paths & determinism -----------------------------------------

TEST(EmphasisPlannerTest, EmptyInputYieldsEmptyPlan) {
    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan({}, {});
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().clicks().empty());
    EXPECT_TRUE(result.value().hideSpans().empty());
}

TEST(EmphasisPlannerTest, RejectsUnsortedMoves) {
    const std::vector<CursorMoveEvent> moves{moveAt(0.5, 0.5, 100), moveAt(0.5, 0.5, 50)};
    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan(moves, {});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(EmphasisPlannerTest, RejectsUnsortedClicks) {
    const std::vector<CursorClickEvent> clicks{clickAt(0.5, 0.5, 100, CursorButton::Left),
                                               clickAt(0.5, 0.5, 50, CursorButton::Left)};
    const EmphasisPlanner planner{defaults()};
    const auto result = planner.plan({}, clicks);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(EmphasisPlannerTest, DeterministicAcrossRuns) {
    std::vector<CursorMoveEvent> moves;
    for (std::int64_t ms = 0; ms <= 2600; ms += 100) {
        moves.push_back(moveAt(0.4, 0.6, ms));
    }
    const std::vector<CursorClickEvent> clicks{clickAt(0.4, 0.6, 300, CursorButton::Left),
                                               clickAt(0.4, 0.6, 1200, CursorButton::Right)};

    const EmphasisPlanner planner{defaults()};
    const auto first = planner.plan(moves, clicks);
    const auto second = planner.plan(moves, clicks);
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value(), second.value());
}

}  // namespace
