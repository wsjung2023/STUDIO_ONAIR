#include "domain/TimelineTypes.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::domain::AssetId;
using creator::domain::AudioEnvelope;
using creator::domain::ClipId;
using creator::domain::TimeRange;
using creator::domain::VisualTransform;

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{DurationNs{nanoseconds}};
}

static_assert(!std::same_as<AssetId, ClipId>);

TEST(TimeRangeTest, CreatesPositiveHalfOpenRange) {
    const auto created = TimeRange::create(at(10), DurationNs{5});

    ASSERT_TRUE(created.hasValue());
    EXPECT_EQ(created.value().start(), at(10));
    EXPECT_EQ(created.value().duration(), DurationNs{5});
    EXPECT_EQ(created.value().end(), at(15));
}

TEST(TimeRangeTest, RejectsNegativeStartAndNonPositiveDuration) {
    const auto negativeStart = TimeRange::create(at(-1), DurationNs{1});
    const auto zeroDuration = TimeRange::create(at(0), DurationNs::zero());
    const auto negativeDuration = TimeRange::create(at(0), DurationNs{-1});

    ASSERT_FALSE(negativeStart.hasValue());
    EXPECT_EQ(negativeStart.error().code(), ErrorCode::InvalidArgument);
    ASSERT_FALSE(zeroDuration.hasValue());
    EXPECT_EQ(zeroDuration.error().code(), ErrorCode::InvalidArgument);
    ASSERT_FALSE(negativeDuration.hasValue());
    EXPECT_EQ(negativeDuration.error().code(), ErrorCode::InvalidArgument);
}

TEST(TimeRangeTest, RejectsEndOverflow) {
    const auto created = TimeRange::create(
        at(std::numeric_limits<std::int64_t>::max() - 1), DurationNs{2});

    ASSERT_FALSE(created.hasValue());
    EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);
}

TEST(TimeRangeTest, UsesHalfOpenOverlapSemantics) {
    const auto first = TimeRange::create(at(10), DurationNs{5}).value();
    const auto touching = TimeRange::create(at(15), DurationNs{3}).value();
    const auto overlapping = TimeRange::create(at(14), DurationNs{3}).value();

    EXPECT_FALSE(overlaps(first, touching));
    EXPECT_FALSE(overlaps(touching, first));
    EXPECT_TRUE(overlaps(first, overlapping));
    EXPECT_TRUE(overlaps(overlapping, first));
}

TEST(VisualTransformTest, CreatesFiniteNormalizedTransform) {
    const auto created = VisualTransform::create(
        0.25, 0.2, 0.5, 0.4, 1.2, 0.8, 15.0,
        0.1, 0.05, 0.2, 0.1, 0.75, 3);

    ASSERT_TRUE(created.hasValue());
    EXPECT_DOUBLE_EQ(created.value().x(), 0.25);
    EXPECT_DOUBLE_EQ(created.value().y(), 0.2);
    EXPECT_DOUBLE_EQ(created.value().width(), 0.5);
    EXPECT_DOUBLE_EQ(created.value().height(), 0.4);
    EXPECT_DOUBLE_EQ(created.value().scaleX(), 1.2);
    EXPECT_DOUBLE_EQ(created.value().scaleY(), 0.8);
    EXPECT_DOUBLE_EQ(created.value().rotationDegrees(), 15.0);
    EXPECT_DOUBLE_EQ(created.value().opacity(), 0.75);
    EXPECT_EQ(created.value().zOrder(), 3);
}

TEST(VisualTransformTest, RejectsInvalidBoundsAndNonFiniteValues) {
    const auto zeroWidth = VisualTransform::create(
        0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 0);
    const auto zeroScale = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 0);
    const auto excessiveCrop = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.6, 0.0, 0.4, 0.0, 1.0, 0);
    const auto excessiveOpacity = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.1, 0);
    const auto nanRotation = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0,
        std::numeric_limits<double>::quiet_NaN(),
        0.0, 0.0, 0.0, 0.0, 1.0, 0);

    EXPECT_FALSE(zeroWidth.hasValue());
    EXPECT_FALSE(zeroScale.hasValue());
    EXPECT_FALSE(excessiveCrop.hasValue());
    EXPECT_FALSE(excessiveOpacity.hasValue());
    EXPECT_FALSE(nanRotation.hasValue());
}

TEST(AudioEnvelopeTest, CreatesBoundedGainAndFades) {
    const auto created = AudioEnvelope::create(
        -6.0, DurationNs{1}, DurationNs{2}, DurationNs{10});

    ASSERT_TRUE(created.hasValue());
    EXPECT_DOUBLE_EQ(created.value().gainDb(), -6.0);
    EXPECT_EQ(created.value().fadeIn(), DurationNs{1});
    EXPECT_EQ(created.value().fadeOut(), DurationNs{2});
}

TEST(AudioEnvelopeTest, RejectsInvalidGainAndFadeDurations) {
    const auto excessiveGain = AudioEnvelope::create(
        24.1, DurationNs::zero(), DurationNs::zero(), DurationNs{1});
    const auto negativeFade = AudioEnvelope::create(
        0.0, DurationNs{-1}, DurationNs::zero(), DurationNs{1});
    const auto overlappingFades = AudioEnvelope::create(
        0.0, DurationNs{6}, DurationNs{5}, DurationNs{10});
    const auto nanGain = AudioEnvelope::create(
        std::numeric_limits<double>::quiet_NaN(), DurationNs::zero(),
        DurationNs::zero(), DurationNs{1});

    EXPECT_FALSE(excessiveGain.hasValue());
    EXPECT_FALSE(negativeFade.hasValue());
    EXPECT_FALSE(overlappingFades.hasValue());
    EXPECT_FALSE(nanGain.hasValue());
}

}  // namespace
