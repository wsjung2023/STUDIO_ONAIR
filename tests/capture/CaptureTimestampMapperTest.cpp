#include "capture/CaptureTimestampMapper.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace {

using creator::capture::CaptureTimestampMapper;
using creator::capture::NativeTimestamp;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::TimestampNs;

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{ProjectClock::duration{nanoseconds}};
}

TEST(CaptureTimestampMapperTest, AnchorsFirstNativeSampleToProjectTime) {
    CaptureTimestampMapper mapper{at(5'000'000'000)};

    const auto mapped = mapper.map(NativeTimestamp{.value = 9'000, .timescale = 600});

    ASSERT_TRUE(mapped.hasValue());
    EXPECT_EQ(mapped.value(), at(5'000'000'000));
}

TEST(CaptureTimestampMapperTest, ConvertsExactRational60FpsWithoutFloatingPoint) {
    CaptureTimestampMapper mapper{at(2'000)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 50, .timescale = 60}).hasValue());

    const auto mapped = mapper.map(NativeTimestamp{.value = 51, .timescale = 60});

    ASSERT_TRUE(mapped.hasValue());
    EXPECT_EQ(mapped.value(), at(16'668'666));
}

TEST(CaptureTimestampMapperTest, ConvertsExactRational5994FpsCadence) {
    CaptureTimestampMapper mapper{at(0)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 0, .timescale = 60'000}).hasValue());

    const auto mapped = mapper.map(NativeTimestamp{.value = 1'001, .timescale = 60'000});

    ASSERT_TRUE(mapped.hasValue());
    EXPECT_EQ(mapped.value(), at(16'683'333));
}

TEST(CaptureTimestampMapperTest, RejectsNonPositiveTimescale) {
    CaptureTimestampMapper mapper{at(0)};

    for (const std::int32_t timescale : {0, -1}) {
        const auto mapped = mapper.map(NativeTimestamp{.value = 0, .timescale = timescale});
        ASSERT_FALSE(mapped.hasValue());
        EXPECT_EQ(mapped.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(CaptureTimestampMapperTest, RejectsTimescaleChangeWithinStream) {
    CaptureTimestampMapper mapper{at(0)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 0, .timescale = 600}).hasValue());

    const auto mapped = mapper.map(NativeTimestamp{.value = 1, .timescale = 60});

    ASSERT_FALSE(mapped.hasValue());
    EXPECT_EQ(mapped.error().code(), ErrorCode::InvalidArgument);
}

TEST(CaptureTimestampMapperTest, RejectsTimestampThatMovesBackward) {
    CaptureTimestampMapper mapper{at(0)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 100, .timescale = 600}).hasValue());
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 102, .timescale = 600}).hasValue());

    const auto mapped = mapper.map(NativeTimestamp{.value = 101, .timescale = 600});

    ASSERT_FALSE(mapped.hasValue());
    EXPECT_EQ(mapped.error().code(), ErrorCode::InvalidArgument);
}

TEST(CaptureTimestampMapperTest, AcceptsDuplicatePresentationTimestamp) {
    CaptureTimestampMapper mapper{at(17)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 10, .timescale = 600}).hasValue());

    const auto duplicate = mapper.map(NativeTimestamp{.value = 10, .timescale = 600});

    ASSERT_TRUE(duplicate.hasValue());
    EXPECT_EQ(duplicate.value(), at(17));
}

TEST(CaptureTimestampMapperTest, RejectsNativeDeltaThatCannotFitProjectTimeline) {
    CaptureTimestampMapper mapper{at(0)};
    ASSERT_TRUE(mapper
                    .map(NativeTimestamp{.value = std::numeric_limits<std::int64_t>::min(),
                                         .timescale = 1})
                    .hasValue());

    const auto mapped = mapper.map(
        NativeTimestamp{.value = std::numeric_limits<std::int64_t>::max(), .timescale = 1});

    ASSERT_FALSE(mapped.hasValue());
    EXPECT_EQ(mapped.error().code(), ErrorCode::InvalidArgument);
}

TEST(CaptureTimestampMapperTest, RejectsProjectAnchorAdditionOverflow) {
    CaptureTimestampMapper mapper{at(std::numeric_limits<std::int64_t>::max() - 5)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 0, .timescale = 1'000'000'000}).hasValue());

    const auto mapped =
        mapper.map(NativeTimestamp{.value = 10, .timescale = 1'000'000'000});

    ASSERT_FALSE(mapped.hasValue());
    EXPECT_EQ(mapped.error().code(), ErrorCode::InvalidArgument);
}

TEST(CaptureTimestampMapperTest, ResetStartsACompletelyNewNativeEpoch) {
    CaptureTimestampMapper mapper{at(0)};
    ASSERT_TRUE(mapper.map(NativeTimestamp{.value = 100, .timescale = 600}).hasValue());

    mapper.reset(at(42));
    const auto mapped = mapper.map(NativeTimestamp{.value = -7, .timescale = 1'000});

    ASSERT_TRUE(mapped.hasValue());
    EXPECT_EQ(mapped.value(), at(42));
}

}  // namespace

