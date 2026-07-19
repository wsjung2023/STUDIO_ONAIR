#include "autozoom/AutoZoomParameters.h"
#include "autozoom/ZoomCandidate.h"
#include "autozoom/ZoomRegion.h"

#include "core/Timebase.h"
#include "cursor/CursorPoint.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>

namespace {

using creator::autozoom::AutoZoomParameters;
using creator::autozoom::ZoomCandidate;
using creator::autozoom::ZoomRegion;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::cursor::CursorPoint;
using creator::domain::TimeRange;

CursorPoint pointAt(double x, double y) {
    return CursorPoint::create(x, y).value();
}

TimestampNs at(std::int64_t ns) {
    return TimestampNs{DurationNs{ns}};
}

// ---- ZoomRegion --------------------------------------------------------

TEST(ZoomRegionTest, AcceptsCenteredZoomInsideFrame) {
    const auto region = ZoomRegion::create(pointAt(0.5, 0.5), 2.0);
    ASSERT_TRUE(region.hasValue());
    EXPECT_DOUBLE_EQ(region.value().zoomFactor(), 2.0);
    EXPECT_DOUBLE_EQ(region.value().visibleWidth(), 0.5);
    EXPECT_DOUBLE_EQ(region.value().visibleX(), 0.25);
    EXPECT_DOUBLE_EQ(region.value().visibleY(), 0.25);
}

TEST(ZoomRegionTest, RejectsFactorBelowOne) {
    const auto region = ZoomRegion::create(pointAt(0.5, 0.5), 0.9);
    ASSERT_FALSE(region.hasValue());
    EXPECT_EQ(region.error().code(), ErrorCode::InvalidArgument);
}

TEST(ZoomRegionTest, RejectsNonFiniteFactor) {
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(ZoomRegion::create(pointAt(0.5, 0.5), inf).hasValue());
}

TEST(ZoomRegionTest, RejectsViewportEscapingFrame) {
    // Center near a corner with a strong factor pushes the viewport past 0.
    const auto region = ZoomRegion::create(pointAt(0.05, 0.05), 2.5);
    ASSERT_FALSE(region.hasValue());
    EXPECT_EQ(region.error().code(), ErrorCode::InvalidArgument);
}

TEST(ZoomRegionTest, AcceptsEdgeAlignedViewport) {
    // factor 2 -> half 0.25; center 0.25 puts the viewport exactly on 0.
    EXPECT_TRUE(ZoomRegion::create(pointAt(0.25, 0.75), 2.0).hasValue());
}

// ---- ZoomCandidate -----------------------------------------------------

ZoomRegion someRegion() {
    return ZoomRegion::create(pointAt(0.5, 0.5), 2.0).value();
}

TimeRange someSpan() {
    return TimeRange::create(at(1'000), DurationNs{2'000}).value();
}

TEST(ZoomCandidateTest, AcceptsScoreInRange) {
    const auto candidate = ZoomCandidate::create(someSpan(), someRegion(), 0.5);
    ASSERT_TRUE(candidate.hasValue());
    EXPECT_DOUBLE_EQ(candidate.value().score(), 0.5);
    EXPECT_EQ(candidate.value().span(), someSpan());
    EXPECT_EQ(candidate.value().region(), someRegion());
}

TEST(ZoomCandidateTest, AcceptsScoreBounds) {
    EXPECT_TRUE(ZoomCandidate::create(someSpan(), someRegion(), 0.0).hasValue());
    EXPECT_TRUE(ZoomCandidate::create(someSpan(), someRegion(), 1.0).hasValue());
}

TEST(ZoomCandidateTest, RejectsScoreOutOfRange) {
    EXPECT_FALSE(ZoomCandidate::create(someSpan(), someRegion(), -0.01).hasValue());
    const auto over = ZoomCandidate::create(someSpan(), someRegion(), 1.01);
    ASSERT_FALSE(over.hasValue());
    EXPECT_EQ(over.error().code(), ErrorCode::InvalidArgument);
}

TEST(ZoomCandidateTest, RejectsNonFiniteScore) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(ZoomCandidate::create(someSpan(), someRegion(), nan).hasValue());
}

// ---- AutoZoomParameters ------------------------------------------------

TEST(AutoZoomParametersTest, DefaultsAreValid) {
    const auto params = AutoZoomParameters::create();
    ASSERT_TRUE(params.hasValue());
    EXPECT_EQ(params.value().minDwellDuration(),
              AutoZoomParameters::kDefaultMinDwellDuration);
    EXPECT_DOUBLE_EQ(params.value().focusRadius(), 0.08);
    EXPECT_DOUBLE_EQ(params.value().maxZoomFactor(), 2.5);
}

TEST(AutoZoomParametersTest, RejectsNonPositiveDwell) {
    const auto params = AutoZoomParameters::create(DurationNs{0});
    ASSERT_FALSE(params.hasValue());
    EXPECT_EQ(params.error().code(), ErrorCode::InvalidArgument);
}

TEST(AutoZoomParametersTest, RejectsRadiusOutOfRange) {
    EXPECT_FALSE(AutoZoomParameters::create(DurationNs{1}, 0.0).hasValue());
    EXPECT_FALSE(AutoZoomParameters::create(DurationNs{1}, 1.5).hasValue());
}

TEST(AutoZoomParametersTest, RejectsFactorBelowOne) {
    EXPECT_FALSE(AutoZoomParameters::create(DurationNs{1}, 0.1, 0.5).hasValue());
}

TEST(AutoZoomParametersTest, RejectsNegativeGapAndClickWeight) {
    EXPECT_FALSE(AutoZoomParameters::create(DurationNs{1}, 0.1, 2.0, DurationNs{-1}).hasValue());
    EXPECT_FALSE(
        AutoZoomParameters::create(DurationNs{1}, 0.1, 2.0, DurationNs{0}, -0.1).hasValue());
}

}  // namespace
