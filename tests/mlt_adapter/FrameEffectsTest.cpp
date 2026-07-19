#include "mlt_adapter/FrameEffects.h"

#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using namespace creator;

domain::VisualTransform visual(double x = 0.0, double y = 0.0,
                               double width = 1.0, double height = 1.0,
                               double scaleX = 1.0, double scaleY = 1.0,
                               double rotation = 0.0,
                               double cropLeft = 0.0, double cropTop = 0.0,
                               double cropRight = 0.0,
                               double cropBottom = 0.0,
                               double opacity = 1.0) {
    return domain::VisualTransform::create(
               x, y, width, height, scaleX, scaleY, rotation, cropLeft,
               cropTop, cropRight, cropBottom, opacity, 0)
        .value();
}

mlt_adapter::BgraFrameView view(const std::vector<std::uint8_t>& bytes,
                                std::uint32_t width,
                                std::uint32_t height) {
    return {std::span<const std::uint8_t>{bytes}, width, height, width * 4U};
}

TEST(FrameEffectsTest, IdentityTransformAliasesInputWithoutAllocation) {
    const std::vector<std::uint8_t> pixels{
        1, 2, 3, 255, 4, 5, 6, 200,
        7, 8, 9, 100, 10, 11, 12, 50};

    auto result = mlt_adapter::applyVisualTransform(
        view(pixels, 2, 2), 2, 2, visual());

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(result.value().aliasesInput());
    EXPECT_EQ(result.value().bytes().data(), pixels.data());
    EXPECT_EQ(result.value().bytes().size(), pixels.size());
}

TEST(FrameEffectsTest, TranslatesAndAppliesOpacityOverTransparentPadding) {
    const std::vector<std::uint8_t> pixel{10, 20, 30, 200};

    auto result = mlt_adapter::applyVisualTransform(
        view(pixel, 1, 1), 2, 1,
        visual(0.5, 0.0, 0.5, 1.0, 1.0, 1.0, 0.0,
               0.0, 0.0, 0.0, 0.0, 0.5));

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_FALSE(result.value().aliasesInput());
    const std::vector<std::uint8_t> expected{0, 0, 0, 0, 5, 10, 15, 100};
    EXPECT_EQ(std::vector(result.value().bytes().begin(),
                          result.value().bytes().end()), expected);
}

TEST(FrameEffectsTest, BilinearSamplingCropAndHalfTurnAreDeterministic) {
    const std::vector<std::uint8_t> twoPixels{
        0, 0, 0, 255, 100, 200, 50, 255};
    auto averaged = mlt_adapter::applyVisualTransform(
        view(twoPixels, 2, 1), 1, 1, visual());
    ASSERT_TRUE(averaged.hasValue()) << averaged.error().message();
    EXPECT_EQ(averaged.value().bytes()[0], 50);
    EXPECT_EQ(averaged.value().bytes()[1], 100);
    EXPECT_EQ(averaged.value().bytes()[2], 25);
    EXPECT_EQ(averaged.value().bytes()[3], 255);

    const std::vector<std::uint8_t> fourPixels{
        1, 0, 0, 255, 2, 0, 0, 255,
        3, 0, 0, 255, 4, 0, 0, 255};
    auto cropped = mlt_adapter::applyVisualTransform(
        view(fourPixels, 4, 1), 2, 1,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
               0.5, 0.0, 0.0, 0.0));
    ASSERT_TRUE(cropped.hasValue()) << cropped.error().message();
    EXPECT_EQ(cropped.value().bytes()[0], 3);
    EXPECT_EQ(cropped.value().bytes()[4], 4);

    auto rotated = mlt_adapter::applyVisualTransform(
        view(twoPixels, 2, 1), 2, 1,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 180.0));
    ASSERT_TRUE(rotated.hasValue()) << rotated.error().message();
    EXPECT_EQ(rotated.value().bytes()[0], 100);
    EXPECT_EQ(rotated.value().bytes()[4], 0);
}

TEST(FrameEffectsTest, CropsEveryEdgeUsingSourcePixelCoordinates) {
    std::vector<std::uint8_t> pixels;
    for (std::uint8_t value = 1; value <= 9; ++value) {
        pixels.insert(pixels.end(), {value, 0, 0, 255});
    }
    const auto channel = [](const mlt_adapter::ProcessedBgraFrame& frame,
                            std::size_t pixel) {
        return frame.bytes()[pixel * 4U];
    };
    constexpr double third = 1.0 / 3.0;

    auto left = mlt_adapter::applyVisualTransform(
        view(pixels, 3, 3), 2, 3,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
               third, 0.0, 0.0, 0.0));
    auto right = mlt_adapter::applyVisualTransform(
        view(pixels, 3, 3), 2, 3,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
               0.0, 0.0, third, 0.0));
    auto top = mlt_adapter::applyVisualTransform(
        view(pixels, 3, 3), 3, 2,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
               0.0, third, 0.0, 0.0));
    auto bottom = mlt_adapter::applyVisualTransform(
        view(pixels, 3, 3), 3, 2,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
               0.0, 0.0, 0.0, third));

    ASSERT_TRUE(left.hasValue());
    ASSERT_TRUE(right.hasValue());
    ASSERT_TRUE(top.hasValue());
    ASSERT_TRUE(bottom.hasValue());
    EXPECT_EQ(channel(left.value(), 0), 2);
    EXPECT_EQ(channel(left.value(), 1), 3);
    EXPECT_EQ(channel(right.value(), 0), 1);
    EXPECT_EQ(channel(right.value(), 1), 2);
    EXPECT_EQ(channel(top.value(), 0), 4);
    EXPECT_EQ(channel(top.value(), 3), 7);
    EXPECT_EQ(channel(bottom.value(), 0), 1);
    EXPECT_EQ(channel(bottom.value(), 3), 4);
}

TEST(FrameEffectsTest, InverseRotationHandlesQuarterAndNonRightAngles) {
    const std::vector<std::uint8_t> asymmetric{
        1, 0, 0, 255, 2, 0, 0, 255,
        3, 0, 0, 255, 4, 0, 0, 255};
    auto quarter = mlt_adapter::applyVisualTransform(
        view(asymmetric, 2, 2), 2, 2,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 90.0));
    ASSERT_TRUE(quarter.hasValue()) << quarter.error().message();
    EXPECT_EQ(quarter.value().bytes()[0], 3);
    EXPECT_EQ(quarter.value().bytes()[4], 1);
    EXPECT_EQ(quarter.value().bytes()[8], 4);
    EXPECT_EQ(quarter.value().bytes()[12], 2);

    std::vector<std::uint8_t> opaque(4U * 4U * 4U, 255U);
    auto first = mlt_adapter::applyVisualTransform(
        view(opaque, 4, 4), 4, 4,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 37.0));
    auto second = mlt_adapter::applyVisualTransform(
        view(opaque, 4, 4), 4, 4,
        visual(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 37.0));
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_TRUE(std::equal(first.value().bytes().begin(),
                           first.value().bytes().end(),
                           second.value().bytes().begin(),
                           second.value().bytes().end()));
    EXPECT_EQ(first.value().bytes()[3], 0);
}

TEST(FrameEffectsTest, RejectsMalformedBuffersAndOverflowingCanvas) {
    const std::vector<std::uint8_t> shortBuffer(7, 0);
    EXPECT_FALSE(mlt_adapter::applyVisualTransform(
                     {shortBuffer, 2, 1, 8}, 2, 1, visual())
                     .hasValue());
    EXPECT_FALSE(mlt_adapter::applyVisualTransform(
                     {shortBuffer, 1, 1, 3}, 1, 1, visual())
                     .hasValue());
    EXPECT_FALSE(mlt_adapter::applyVisualTransform(
                     {std::span<const std::uint8_t>{shortBuffer}, 1, 1, 4},
                     std::numeric_limits<std::uint32_t>::max(),
                     std::numeric_limits<std::uint32_t>::max(), visual())
                     .hasValue());
}

TEST(FrameEffectsTest, RejectsOverflowingAndUnderflowingDerivedGeometry) {
    const std::vector<std::uint8_t> pixel{10, 20, 30, 255};
    const auto maximum = std::numeric_limits<double>::max();
    const auto minimum = std::numeric_limits<double>::denorm_min();
    const auto overflow = domain::VisualTransform::create(
                              0.0, 0.0, 1.0, 1.0, maximum, 1.0, 0.0,
                              0.0, 0.0, 0.0, 0.0, 1.0, 0)
                              .value();
    const auto underflow = domain::VisualTransform::create(
                               0.0, 0.0, minimum, 1.0, minimum, 1.0, 0.0,
                               0.0, 0.0, 0.0, 0.0, 1.0, 0)
                               .value();

    EXPECT_FALSE(mlt_adapter::applyVisualTransform(
                     view(pixel, 1, 1), 2, 1, overflow)
                     .hasValue());
    EXPECT_FALSE(mlt_adapter::applyVisualTransform(
                     view(pixel, 1, 1), 2, 1, underflow)
                     .hasValue());
}

TEST(FrameEffectsTest, AppliesGainFadesInterleavedAndClipLocalOffset) {
    auto envelope = domain::AudioEnvelope::create(
                        0.0, core::DurationNs{2'000'000},
                        core::DurationNs{2'000'000},
                        core::DurationNs{5'000'000})
                        .value();
    std::vector<float> samples(10, 1.0F);

    auto applied = mlt_adapter::applyAudioEnvelope(
        samples, 2, 0, 5, 1'000, envelope);

    ASSERT_TRUE(applied.hasValue()) << applied.error().message();
    const std::array<float, 5> expected{0.0F, 1.0F, 1.0F, 1.0F, 0.0F};
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        EXPECT_FLOAT_EQ(samples[frame * 2], expected[frame]);
        EXPECT_FLOAT_EQ(samples[frame * 2 + 1], expected[frame]);
    }

    std::vector<float> middle{1.0F, 1.0F};
    ASSERT_TRUE(mlt_adapter::applyAudioEnvelope(
                    middle, 1, 2, 5, 1'000, envelope)
                    .hasValue());
    EXPECT_FLOAT_EQ(middle[0], 1.0F);
    EXPECT_FLOAT_EQ(middle[1], 1.0F);
}

TEST(FrameEffectsTest, ClampsGainAndRejectsInvalidAudioWithoutMutation) {
    auto boosted = domain::AudioEnvelope::create(
                       6.0, core::DurationNs{0}, core::DurationNs{0},
                       core::DurationNs{1'000'000})
                       .value();
    std::vector<float> samples{0.75F, -0.75F};
    ASSERT_TRUE(mlt_adapter::applyAudioEnvelope(
                    samples, 2, 0, 1, 1'000, boosted)
                    .hasValue());
    EXPECT_FLOAT_EQ(samples[0], 1.0F);
    EXPECT_FLOAT_EQ(samples[1], -1.0F);

    std::vector<float> sentinel{0.25F, std::numeric_limits<float>::quiet_NaN()};
    const auto originalFirst = sentinel[0];
    EXPECT_FALSE(mlt_adapter::applyAudioEnvelope(
                     sentinel, 1, 0, 2, 1'000, boosted)
                     .hasValue());
    EXPECT_FLOAT_EQ(sentinel[0], originalFirst);
    EXPECT_TRUE(std::isnan(sentinel[1]));

    std::vector<float> shape{0.5F, 0.5F, 0.5F};
    EXPECT_FALSE(mlt_adapter::applyAudioEnvelope(
                     shape, 2, 0, 1, 1'000, boosted)
                     .hasValue());
    EXPECT_EQ(shape, (std::vector<float>{0.5F, 0.5F, 0.5F}));
}

TEST(FrameEffectsTest, AppliesMinusSixDbAndDoesNotTouchOutsideSpan) {
    auto reduced = domain::AudioEnvelope::create(
                       -6.0, core::DurationNs{0}, core::DurationNs{0},
                       core::DurationNs{4'000'000})
                       .value();
    std::vector<float> storage{9.0F, 1.0F, -1.0F, 0.5F, -0.5F, 8.0F};

    auto applied = mlt_adapter::applyAudioEnvelope(
        std::span<float>{storage}.subspan(1, 4), 1, 0, 4, 1'000, reduced);

    ASSERT_TRUE(applied.hasValue()) << applied.error().message();
    constexpr double expectedGain = 0.5011872336272722;
    EXPECT_FLOAT_EQ(storage[0], 9.0F);
    EXPECT_NEAR(storage[1], expectedGain, 1e-6);
    EXPECT_NEAR(storage[2], -expectedGain, 1e-6);
    EXPECT_NEAR(storage[3], 0.5 * expectedGain, 1e-6);
    EXPECT_NEAR(storage[4], -0.5 * expectedGain, 1e-6);
    EXPECT_FLOAT_EQ(storage[5], 8.0F);
}

}  // namespace
