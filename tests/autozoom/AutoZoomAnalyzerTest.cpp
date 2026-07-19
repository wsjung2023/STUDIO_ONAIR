#include "autozoom/AutoZoomAnalyzer.h"
#include "autozoom/AutoZoomParameters.h"

#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorPoint.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using creator::autozoom::AutoZoomAnalyzer;
using creator::autozoom::AutoZoomParameters;
using creator::autozoom::ZoomCandidate;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorClickEvent;
using creator::cursor::CursorMoveEvent;
using creator::cursor::CursorPoint;
using creator::domain::SourceId;

std::int64_t msToNs(std::int64_t ms) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<DurationNs>(std::chrono::milliseconds{ms}).count());
}

TimestampNs at(std::int64_t ns) {
    return TimestampNs{DurationNs{ns}};
}

SourceId screen() {
    return SourceId::create("screen-1").value();
}

AutoZoomParameters defaults() {
    return AutoZoomParameters::create().value();
}

// Deterministic in-radius jitter so a "dwell" is realistically noisy but tight.
double jitterX(std::size_t i) { return (static_cast<double>(i % 5) - 2.0) * 0.004; }
double jitterY(std::size_t i) { return (static_cast<double>(i % 3) - 1.0) * 0.004; }

// Appends `count` move samples around (cx, cy), one every `stepMs` from startMs.
// Returns the timestamp (ns) just after the last appended sample.
std::int64_t appendDwell(std::vector<CursorMoveEvent>& moves, double cx, double cy,
                         std::size_t count, std::int64_t startMs, std::int64_t stepMs) {
    std::int64_t tMs = startMs;
    for (std::size_t i = 0; i < count; ++i) {
        const auto point = CursorPoint::create(cx + jitterX(i), cy + jitterY(i)).value();
        moves.push_back(CursorMoveEvent::create(at(msToNs(tMs)), point, screen()).value());
        tMs += stepMs;
    }
    return tMs;
}

CursorClickEvent clickAt(double x, double y, std::int64_t ms) {
    return CursorClickEvent::create(at(msToNs(ms)), CursorPoint::create(x, y).value(),
                                    CursorButton::Left)
        .value();
}

// A cursor that dwells tightly in one place for ~2s -> exactly one candidate
// centered on that place, spanning the dwell.
TEST(AutoZoomAnalyzerTest, SingleDwellYieldsOneCenteredCandidate) {
    std::vector<CursorMoveEvent> moves;
    appendDwell(moves, 0.75, 0.25, /*count=*/40, /*startMs=*/0, /*stepMs=*/50);

    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze(moves, {});
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().size(), 1u);

    const ZoomCandidate& candidate = result.value().front();
    EXPECT_NEAR(candidate.region().center().x(), 0.75, 0.03);
    EXPECT_NEAR(candidate.region().center().y(), 0.25, 0.03);
    EXPECT_GT(candidate.region().zoomFactor(), 1.0);
    // The span covers the dwell: starts at/near the first sample and lasts ~2s.
    EXPECT_EQ(candidate.span().start(), at(0));
    EXPECT_GE(candidate.span().duration(), DurationNs{msToNs(1500)});
    EXPECT_GT(candidate.score(), 0.0);
}

// A cursor that roams across the whole frame the whole time -> nothing to zoom.
TEST(AutoZoomAnalyzerTest, BroadRoamYieldsNoCandidates) {
    const double corners[4][2] = {{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.1, 0.9}};
    std::vector<CursorMoveEvent> moves;
    std::int64_t tMs = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const auto& c = corners[i % 4];
        moves.push_back(
            CursorMoveEvent::create(at(msToNs(tMs)), CursorPoint::create(c[0], c[1]).value(),
                                    screen())
                .value());
        tMs += 100;
    }

    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze(moves, {});
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

// Two tight dwells separated by a big time gap -> two ordered candidates.
TEST(AutoZoomAnalyzerTest, TwoSeparatedDwellsYieldTwoOrderedCandidates) {
    std::vector<CursorMoveEvent> moves;
    const std::int64_t afterA = appendDwell(moves, 0.25, 0.25, 30, 0, 50);
    appendDwell(moves, 0.75, 0.75, 30, afterA + 3000, 50);

    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze(moves, {});
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().size(), 2u);

    const auto& first = result.value()[0];
    const auto& second = result.value()[1];
    EXPECT_NEAR(first.region().center().x(), 0.25, 0.03);
    EXPECT_NEAR(second.region().center().x(), 0.75, 0.03);
    // Ordered and strictly non-overlapping.
    EXPECT_LE(first.span().end(), second.span().start());
}

// A tight click cluster qualifies a dwell too short to qualify on time alone,
// and clicks raise the score of a dwell that already qualifies.
TEST(AutoZoomAnalyzerTest, ClicksCanTriggerAndRaiseScore) {
    std::vector<CursorMoveEvent> shortMoves;
    appendDwell(shortMoves, 0.5, 0.5, /*count=*/6, /*startMs=*/0, /*stepMs=*/50);  // ~250ms

    const AutoZoomAnalyzer analyzer{defaults()};

    // Too short, no clicks -> no candidate.
    const auto withoutClicks = analyzer.analyze(shortMoves, {});
    ASSERT_TRUE(withoutClicks.hasValue());
    EXPECT_TRUE(withoutClicks.value().empty());

    // Same short dwell + a tight click cluster -> one candidate.
    std::vector<CursorClickEvent> clicks{clickAt(0.5, 0.5, 50), clickAt(0.5, 0.5, 120),
                                         clickAt(0.5, 0.5, 200)};
    const auto withClicks = analyzer.analyze(shortMoves, clicks);
    ASSERT_TRUE(withClicks.hasValue());
    EXPECT_EQ(withClicks.value().size(), 1u);
}

TEST(AutoZoomAnalyzerTest, ClicksIncreaseScoreOfQualifyingDwell) {
    std::vector<CursorMoveEvent> moves;
    appendDwell(moves, 0.5, 0.5, 30, 0, 50);  // ~1450ms, qualifies on time

    const AutoZoomAnalyzer analyzer{defaults()};
    const auto plain = analyzer.analyze(moves, {});
    ASSERT_TRUE(plain.hasValue());
    ASSERT_EQ(plain.value().size(), 1u);

    std::vector<CursorClickEvent> clicks{clickAt(0.5, 0.5, 300), clickAt(0.5, 0.5, 600)};
    const auto clicked = analyzer.analyze(moves, clicks);
    ASSERT_TRUE(clicked.hasValue());
    ASSERT_EQ(clicked.value().size(), 1u);

    EXPECT_GT(clicked.value().front().score(), plain.value().front().score());
}

// Invariant: candidates are always time-ordered and non-overlapping.
TEST(AutoZoomAnalyzerTest, CandidatesAreOrderedAndNonOverlapping) {
    std::vector<CursorMoveEvent> moves;
    std::int64_t t = appendDwell(moves, 0.3, 0.3, 25, 0, 50);
    t = appendDwell(moves, 0.7, 0.3, 25, t + 2000, 50);
    appendDwell(moves, 0.5, 0.7, 25, t + 2000, 50);

    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze(moves, {});
    ASSERT_TRUE(result.hasValue());
    ASSERT_GE(result.value().size(), 2u);
    for (std::size_t i = 1; i < result.value().size(); ++i) {
        EXPECT_LE(result.value()[i - 1].span().end(), result.value()[i].span().start());
    }
}

// Determinism: identical input -> byte-identical candidates across runs.
TEST(AutoZoomAnalyzerTest, DeterministicAcrossRuns) {
    std::vector<CursorMoveEvent> moves;
    const std::int64_t afterA = appendDwell(moves, 0.25, 0.25, 30, 0, 50);
    appendDwell(moves, 0.75, 0.75, 30, afterA + 3000, 50);

    const AutoZoomAnalyzer analyzer{defaults()};
    const auto first = analyzer.analyze(moves, {});
    const auto second = analyzer.analyze(moves, {});
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value(), second.value());
}

// ---- error paths -------------------------------------------------------

TEST(AutoZoomAnalyzerTest, EmptyInputIsNotAnError) {
    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze({}, {});
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST(AutoZoomAnalyzerTest, RejectsUnsortedMoves) {
    std::vector<CursorMoveEvent> moves{
        CursorMoveEvent::create(at(msToNs(100)), CursorPoint::create(0.5, 0.5).value(), screen())
            .value(),
        CursorMoveEvent::create(at(msToNs(50)), CursorPoint::create(0.5, 0.5).value(), screen())
            .value(),
    };
    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze(moves, {});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(AutoZoomAnalyzerTest, RejectsUnsortedClicks) {
    std::vector<CursorClickEvent> clicks{clickAt(0.5, 0.5, 100), clickAt(0.5, 0.5, 50)};
    const AutoZoomAnalyzer analyzer{defaults()};
    const auto result = analyzer.analyze({}, clicks);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
