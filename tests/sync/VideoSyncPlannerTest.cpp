#include "sync/VideoSyncPlanner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <limits>

namespace {

using creator::core::TimestampNs;
using creator::media::PixelFormat;
using creator::media::VideoFrame;
using creator::synchronization::VideoSyncPlanner;

TimestampNs at(std::chrono::nanoseconds value) { return TimestampNs{value}; }

VideoFrame frame(std::shared_ptr<void> owner = std::make_shared<int>(7)) {
    return VideoFrame{.width = 2,
                      .height = 2,
                      .visibleRect = {0, 0, 2, 2},
                      .contentWidth = 2,
                      .contentHeight = 2,
                      .pixelFormat = PixelFormat::Bgra8,
                      .platformHandle = std::move(owner)};
}

std::unique_ptr<VideoSyncPlanner> planner(
    std::chrono::nanoseconds period = std::chrono::milliseconds{10},
    std::size_t maximumDuplicates = 2) {
    return VideoSyncPlanner::create(period, maximumDuplicates).value();
}

TEST(VideoSyncPlannerTest, FirstFramePassesAtCorrectedTimestamp) {
    auto sync = planner();
    auto result = sync->plan(frame(), at(std::chrono::milliseconds{25}));

    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().frames.size(), 1u);
    EXPECT_EQ(result.value().frames[0].timestamp,
              at(std::chrono::milliseconds{25}));
    EXPECT_EQ(sync->snapshot().framesPassed, 1u);
}

TEST(VideoSyncPlannerTest, DropsFrameOlderThanHalfPeriodBehindGrid) {
    auto sync = planner();
    ASSERT_TRUE(sync->plan(frame(), at({})).hasValue());

    auto late = sync->plan(frame(), at(std::chrono::milliseconds{4}));

    ASSERT_TRUE(late.hasValue());
    EXPECT_TRUE(late.value().frames.empty());
    EXPECT_EQ(sync->snapshot().framesDropped, 1u);
}

TEST(VideoSyncPlannerTest, DuplicatesRetainedFrameAcrossForwardGap) {
    auto sync = planner();
    auto owner = std::make_shared<int>(9);
    ASSERT_TRUE(sync->plan(frame(owner), at({})).hasValue());

    auto gap = sync->plan(frame(), at(std::chrono::milliseconds{30}));

    ASSERT_TRUE(gap.hasValue());
    ASSERT_EQ(gap.value().frames.size(), 3u);
    EXPECT_EQ(gap.value().frames[0].timestamp, at(std::chrono::milliseconds{10}));
    EXPECT_EQ(gap.value().frames[1].timestamp, at(std::chrono::milliseconds{20}));
    EXPECT_EQ(gap.value().frames[2].timestamp, at(std::chrono::milliseconds{30}));
    EXPECT_EQ(gap.value().frames[0].platformHandle, owner);
    EXPECT_EQ(gap.value().frames[1].platformHandle, owner);
    EXPECT_EQ(sync->snapshot().framesDuplicated, 2u);
}

TEST(VideoSyncPlannerTest, CapsDuplicatesAndRecordsSkippedGridIntervals) {
    auto sync = planner(std::chrono::milliseconds{10}, 2);
    ASSERT_TRUE(sync->plan(frame(), at({})).hasValue());

    auto gap = sync->plan(frame(), at(std::chrono::seconds{1}));

    ASSERT_TRUE(gap.hasValue());
    EXPECT_EQ(gap.value().frames.size(), 3u);
    EXPECT_EQ(sync->snapshot().framesDuplicated, 2u);
    EXPECT_GT(sync->snapshot().gridIntervalsSkipped, 90u);
    EXPECT_EQ(gap.value().frames.back().timestamp,
              at(std::chrono::seconds{1}));
}

TEST(VideoSyncPlannerTest, OutputRemainsMonotonicAcrossDropDuplicateAndPass) {
    auto sync = planner();
    auto first = sync->plan(frame(), at({})).value();
    auto dropped = sync->plan(frame(), at(std::chrono::milliseconds{3})).value();
    auto gap = sync->plan(frame(), at(std::chrono::milliseconds{30})).value();
    auto next = sync->plan(frame(), at(std::chrono::milliseconds{40})).value();

    ASSERT_EQ(first.frames.size(), 1u);
    EXPECT_TRUE(dropped.frames.empty());
    ASSERT_EQ(gap.frames.size(), 3u);
    ASSERT_EQ(next.frames.size(), 1u);
    EXPECT_LT(first.frames.back().timestamp, gap.frames.front().timestamp);
    EXPECT_LT(gap.frames.back().timestamp, next.frames.front().timestamp);
}

TEST(VideoSyncPlannerTest, RejectsInvalidPeriodAndZeroDuplicateBound) {
    EXPECT_FALSE(VideoSyncPlanner::create({}, 2).hasValue());
    EXPECT_FALSE(VideoSyncPlanner::create(std::chrono::milliseconds{10}, 0)
                     .hasValue());
}

TEST(VideoSyncPlannerTest, OverflowingGapDoesNotCommitPartialDuplicates) {
    auto sync = planner();
    const auto minimum = at(std::chrono::nanoseconds{
        std::numeric_limits<std::int64_t>::min()});
    const auto maximum = at(std::chrono::nanoseconds{
        std::numeric_limits<std::int64_t>::max()});
    ASSERT_TRUE(sync->plan(frame(), minimum).hasValue());
    const auto before = sync->snapshot();

    EXPECT_FALSE(sync->plan(frame(), maximum).hasValue());
    const auto after = sync->snapshot();

    EXPECT_EQ(after.framesPassed, before.framesPassed);
    EXPECT_EQ(after.framesDuplicated, before.framesDuplicated);
    EXPECT_EQ(after.gridIntervalsSkipped, before.gridIntervalsSkipped);
}

}  // namespace
