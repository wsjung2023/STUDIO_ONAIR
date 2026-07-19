#include "fakes/FakeCursorSource.h"

#include "core/Timebase.h"
#include "cursor/ICursorSource.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::cursor::RawCursorClickSample;
using creator::cursor::RawCursorMoveSample;
using creator::cursor::RawCursorSample;
using creator::fakes::FakeCursorSource;

TimestampNs at(std::int64_t ns) {
    return TimestampNs{DurationNs{ns}};
}

// A short scripted path: three moves along the top edge and one middle click.
std::vector<RawCursorSample> script() {
    return {
        RawCursorSample{RawCursorMoveSample{at(0), 0, 0, 1920, 1080}},
        RawCursorSample{RawCursorMoveSample{at(1'000), 960, 540, 1920, 1080}},
        RawCursorSample{RawCursorClickSample{at(2'000), 960, 540, 1920, 1080, 2}},
        RawCursorSample{RawCursorMoveSample{at(3'000), 1920, 1080, 1920, 1080}},
    };
}

TEST(FakeCursorSourceTest, ReplaysScriptInOrderThenExhausts) {
    FakeCursorSource source{script()};

    std::vector<RawCursorSample> got;
    while (auto sample = source.poll()) {
        got.push_back(*sample);
    }

    ASSERT_EQ(got.size(), 4u);
    EXPECT_TRUE(source.exhausted());
    // poll() past the end keeps returning nullopt.
    EXPECT_FALSE(source.poll().has_value());

    const auto& move0 = std::get<RawCursorMoveSample>(got[0]);
    EXPECT_EQ(move0.x, 0);
    EXPECT_EQ(move0.y, 0);
    const auto& click = std::get<RawCursorClickSample>(got[2]);
    EXPECT_EQ(click.button, 2);
    EXPECT_EQ(click.tNs, at(2'000));
    const auto& move3 = std::get<RawCursorMoveSample>(got[3]);
    EXPECT_EQ(move3.x, 1920);
    EXPECT_EQ(move3.y, 1080);
}

TEST(FakeCursorSourceTest, IsDeterministicAcrossRuns) {
    FakeCursorSource a{script()};
    FakeCursorSource b{script()};

    std::size_t clicks = 0;
    for (;;) {
        const auto sa = a.poll();
        const auto sb = b.poll();
        ASSERT_EQ(sa.has_value(), sb.has_value());
        if (!sa.has_value()) {
            break;
        }
        ASSERT_EQ(sa->index(), sb->index());
        if (std::holds_alternative<RawCursorClickSample>(*sa)) {
            EXPECT_EQ(std::get<RawCursorClickSample>(*sa).button,
                      std::get<RawCursorClickSample>(*sb).button);
            ++clicks;
        } else {
            EXPECT_EQ(std::get<RawCursorMoveSample>(*sa).x,
                      std::get<RawCursorMoveSample>(*sb).x);
            EXPECT_EQ(std::get<RawCursorMoveSample>(*sa).y,
                      std::get<RawCursorMoveSample>(*sb).y);
        }
    }
    EXPECT_EQ(clicks, 1u);
}

}  // namespace
