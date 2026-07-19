#include "cursor_emphasis/ClickEmphasis.h"
#include "cursor_emphasis/CursorHideSpan.h"
#include "cursor_emphasis/EmphasisPlan.h"
#include "cursor_emphasis/EmphasisPlanParameters.h"

#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorPoint.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorPoint;
using creator::cursor_emphasis::ClickEmphasis;
using creator::cursor_emphasis::CursorHideSpan;
using creator::cursor_emphasis::EmphasisPlan;
using creator::cursor_emphasis::EmphasisPlanParameters;
using creator::cursor_emphasis::EmphasisStyle;
using creator::cursor_emphasis::HideReason;
using creator::domain::TimeRange;

CursorPoint pointAt(double x, double y) { return CursorPoint::create(x, y).value(); }

TimestampNs at(std::int64_t ns) { return TimestampNs{DurationNs{ns}}; }

// ---- ClickEmphasis -----------------------------------------------------

TEST(ClickEmphasisTest, AcceptsValidDirective) {
    const auto emphasis = ClickEmphasis::create(pointAt(0.3, 0.7), at(1'000), DurationNs{600},
                                                CursorButton::Left, EmphasisStyle::Ripple, 0.06);
    ASSERT_TRUE(emphasis.hasValue());
    EXPECT_EQ(emphasis.value().position(), pointAt(0.3, 0.7));
    EXPECT_EQ(emphasis.value().startNs(), at(1'000));
    EXPECT_EQ(emphasis.value().duration(), DurationNs{600});
    EXPECT_EQ(emphasis.value().endNs(), at(1'600));
    EXPECT_EQ(emphasis.value().button(), CursorButton::Left);
    EXPECT_EQ(emphasis.value().style(), EmphasisStyle::Ripple);
    EXPECT_DOUBLE_EQ(emphasis.value().radius(), 0.06);
}

TEST(ClickEmphasisTest, RejectsNonPositiveDuration) {
    const auto zero = ClickEmphasis::create(pointAt(0.5, 0.5), at(0), DurationNs{0},
                                            CursorButton::Left, EmphasisStyle::Ripple, 0.06);
    ASSERT_FALSE(zero.hasValue());
    EXPECT_EQ(zero.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(ClickEmphasis::create(pointAt(0.5, 0.5), at(0), DurationNs{-1},
                                       CursorButton::Left, EmphasisStyle::Ripple, 0.06)
                     .hasValue());
}

TEST(ClickEmphasisTest, RejectsRadiusOutOfRange) {
    EXPECT_FALSE(ClickEmphasis::create(pointAt(0.5, 0.5), at(0), DurationNs{600},
                                       CursorButton::Left, EmphasisStyle::Ripple, 0.0)
                     .hasValue());
    EXPECT_FALSE(ClickEmphasis::create(pointAt(0.5, 0.5), at(0), DurationNs{600},
                                       CursorButton::Left, EmphasisStyle::Ripple, 1.5)
                     .hasValue());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(ClickEmphasis::create(pointAt(0.5, 0.5), at(0), DurationNs{600},
                                       CursorButton::Left, EmphasisStyle::Ripple, nan)
                     .hasValue());
}

TEST(ClickEmphasisTest, RejectsNegativeStart) {
    EXPECT_FALSE(ClickEmphasis::create(pointAt(0.5, 0.5), at(-1), DurationNs{600},
                                       CursorButton::Left, EmphasisStyle::Ripple, 0.06)
                     .hasValue());
}

TEST(EmphasisStyleTest, RoundTripsTokens) {
    EXPECT_EQ(creator::cursor_emphasis::toString(EmphasisStyle::Ripple), "ripple");
    EXPECT_EQ(creator::cursor_emphasis::toString(EmphasisStyle::Highlight), "highlight");
    EXPECT_EQ(creator::cursor_emphasis::emphasisStyleFromString("highlight").value(),
              EmphasisStyle::Highlight);
    EXPECT_FALSE(creator::cursor_emphasis::emphasisStyleFromString("sparkle").hasValue());
}

// ---- CursorHideSpan ----------------------------------------------------

TEST(CursorHideSpanTest, AcceptsValidSpan) {
    const auto range = TimeRange::create(at(1'000), DurationNs{2'000}).value();
    const auto span = CursorHideSpan::create(range, HideReason::Idle);
    ASSERT_TRUE(span.hasValue());
    EXPECT_EQ(span.value().span(), range);
    EXPECT_EQ(span.value().reason(), HideReason::Idle);
}

TEST(CursorHideSpanTest, TimeRangeEnforcesPositiveDuration) {
    // The positive-duration invariant lives in TimeRange; a zero-length range
    // cannot even be built to hand to CursorHideSpan.
    EXPECT_FALSE(TimeRange::create(at(1'000), DurationNs{0}).hasValue());
}

TEST(HideReasonTest, RoundTripsTokens) {
    EXPECT_EQ(creator::cursor_emphasis::toString(HideReason::Idle), "idle");
    EXPECT_EQ(creator::cursor_emphasis::toString(HideReason::ExplicitRegion), "explicit_region");
    EXPECT_EQ(creator::cursor_emphasis::hideReasonFromString("explicit_region").value(),
              HideReason::ExplicitRegion);
    EXPECT_FALSE(creator::cursor_emphasis::hideReasonFromString("nope").hasValue());
}

// ---- EmphasisPlanParameters --------------------------------------------

TEST(EmphasisPlanParametersTest, DefaultsAreValid) {
    const auto params = EmphasisPlanParameters::create();
    ASSERT_TRUE(params.hasValue());
    EXPECT_EQ(params.value().clickEmphasisDuration(),
              EmphasisPlanParameters::kDefaultClickEmphasisDuration);
    EXPECT_DOUBLE_EQ(params.value().emphasisRadius(), 0.06);
    EXPECT_EQ(params.value().idleThreshold(),
              EmphasisPlanParameters::kDefaultIdleThreshold);
    EXPECT_DOUBLE_EQ(params.value().minMovementRadius(), 0.01);
    EXPECT_EQ(params.value().clickEmphasisStyle(), EmphasisStyle::Ripple);
}

TEST(EmphasisPlanParametersTest, RejectsInvalidTunables) {
    EXPECT_FALSE(EmphasisPlanParameters::create(DurationNs{0}).hasValue());
    EXPECT_FALSE(EmphasisPlanParameters::create(DurationNs{600}, 0.0).hasValue());
    EXPECT_FALSE(EmphasisPlanParameters::create(DurationNs{600}, 1.5).hasValue());
    EXPECT_FALSE(EmphasisPlanParameters::create(DurationNs{600}, 0.06,
                                                EmphasisStyle::Ripple, DurationNs{0})
                     .hasValue());
    EXPECT_FALSE(EmphasisPlanParameters::create(DurationNs{600}, 0.06,
                                                EmphasisStyle::Ripple, DurationNs{1}, 0.0)
                     .hasValue());
    EXPECT_FALSE(EmphasisPlanParameters::create(DurationNs{600}, 0.06,
                                                EmphasisStyle::Ripple, DurationNs{1}, 1.5)
                     .hasValue());
}

// ---- EmphasisPlan ------------------------------------------------------

ClickEmphasis clickAt(std::int64_t ns) {
    return ClickEmphasis::create(pointAt(0.5, 0.5), at(ns), DurationNs{600}, CursorButton::Left,
                                 EmphasisStyle::Ripple, 0.06)
        .value();
}

CursorHideSpan hideAt(std::int64_t startNs, std::int64_t durNs) {
    return CursorHideSpan::create(TimeRange::create(at(startNs), DurationNs{durNs}).value(),
                                  HideReason::Idle)
        .value();
}

TEST(EmphasisPlanTest, AcceptsOrderedNonOverlapping) {
    const auto plan = EmphasisPlan::create({clickAt(0), clickAt(100)},
                                           {hideAt(0, 100), hideAt(100, 100)});
    ASSERT_TRUE(plan.hasValue());
    EXPECT_EQ(plan.value().clicks().size(), 2u);
    EXPECT_EQ(plan.value().hideSpans().size(), 2u);
}

TEST(EmphasisPlanTest, EmptyPlanIsValid) {
    const auto plan = EmphasisPlan::create({}, {});
    ASSERT_TRUE(plan.hasValue());
    EXPECT_TRUE(plan.value().clicks().empty());
    EXPECT_TRUE(plan.value().hideSpans().empty());
}

TEST(EmphasisPlanTest, RejectsUnorderedClicks) {
    const auto plan = EmphasisPlan::create({clickAt(100), clickAt(0)}, {});
    ASSERT_FALSE(plan.hasValue());
    EXPECT_EQ(plan.error().code(), ErrorCode::InvalidArgument);
}

TEST(EmphasisPlanTest, RejectsOverlappingHideSpans) {
    const auto plan = EmphasisPlan::create({}, {hideAt(0, 200), hideAt(100, 200)});
    ASSERT_FALSE(plan.hasValue());
    EXPECT_EQ(plan.error().code(), ErrorCode::InvalidArgument);
}

TEST(EmphasisPlanTest, AllowsTouchingHideSpans) {
    // Spans that touch at an instant (start == previous end) do not overlap.
    const auto plan = EmphasisPlan::create({}, {hideAt(0, 100), hideAt(100, 100)});
    EXPECT_TRUE(plan.hasValue());
}

}  // namespace
