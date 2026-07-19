#include "mlt_adapter/CursorVisualEffects.h"

#include "autozoom/ZoomCandidate.h"
#include "autozoom/ZoomRegion.h"
#include "core/Timebase.h"
#include "cursor/CursorPoint.h"
#include "cursor_emphasis/ClickEmphasis.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using creator::autozoom::ZoomCandidate;
using creator::autozoom::ZoomRegion;
using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorPoint;
using creator::cursor_emphasis::ClickEmphasis;
using creator::cursor_emphasis::EmphasisStyle;
using creator::mlt_adapter::BgraFrameView;
using creator::mlt_adapter::CursorVisualEffectsPlan;
using creator::mlt_adapter::applyCursorVisualEffects;

TimestampNs at(std::int64_t ns) { return TimestampNs{DurationNs{ns}}; }

TEST(CursorVisualEffectsTest, DrawsActiveClickHighlightOnTheFrame) {
    std::vector<std::uint8_t> pixels(32U * 32U * 4U, 0U);
    auto emphasis = ClickEmphasis::create(
        CursorPoint::create(0.5, 0.5).value(), at(0), DurationNs{1'000'000'000},
        CursorButton::Left, EmphasisStyle::Highlight, 0.25);
    ASSERT_TRUE(emphasis.hasValue());

    CursorVisualEffectsPlan plan;
    plan.clicks.push_back(emphasis.value());
    auto rendered = applyCursorVisualEffects(
        BgraFrameView{pixels, 32, 32, 32U * 4U}, 32, 32, at(500'000'000), plan);
    ASSERT_TRUE(rendered.hasValue());
    EXPECT_FALSE(rendered.value().aliasesInput());
    const auto bytes = rendered.value().bytes();
    EXPECT_GT(bytes[(16U * 32U + 23U) * 4U + 3U], 0U);
    EXPECT_EQ(bytes[(16U * 32U + 16U) * 4U + 3U], 0U);
}

TEST(CursorVisualEffectsTest, AppliesActiveZoomCandidateBeforeHighlight) {
    std::vector<std::uint8_t> pixels(8U * 8U * 4U, 0U);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * 8U + x) * 4U;
            pixels[offset] = static_cast<std::uint8_t>(x * 20U);
            pixels[offset + 3U] = 255U;
        }
    }
    const auto region = ZoomRegion::create(CursorPoint::create(0.5, 0.5).value(), 2.0);
    ASSERT_TRUE(region.hasValue());
    const auto span = creator::domain::TimeRange::create(
        at(0), DurationNs{1'000'000'000});
    ASSERT_TRUE(span.hasValue());
    const auto candidate = ZoomCandidate::create(span.value(), region.value(), 1.0);
    ASSERT_TRUE(candidate.hasValue());

    CursorVisualEffectsPlan plan;
    plan.zooms.push_back(candidate.value());
    const auto rendered = applyCursorVisualEffects(
        BgraFrameView{pixels, 8, 8, 8U * 4U}, 8, 8, at(500'000'000), plan);
    ASSERT_TRUE(rendered.hasValue());
    EXPECT_EQ(rendered.value().width(), 8U);
    EXPECT_EQ(rendered.value().height(), 8U);
    EXPECT_FALSE(rendered.value().aliasesInput());
    EXPECT_EQ(rendered.value().bytes()[3U], 255U);
}

}  // namespace
