#include "cursor/CursorNormalizer.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

namespace {

using creator::core::ErrorCode;
using creator::cursor::CursorNormalizer;

TEST(CursorNormalizerTest, TopLeftMapsToOrigin) {
    const auto point = CursorNormalizer::normalize(0, 0, 1920, 1080);
    ASSERT_TRUE(point.hasValue());
    EXPECT_DOUBLE_EQ(point.value().x(), 0.0);
    EXPECT_DOUBLE_EQ(point.value().y(), 0.0);
}

TEST(CursorNormalizerTest, FullExtentMapsToOne) {
    const auto point = CursorNormalizer::normalize(1920, 1080, 1920, 1080);
    ASSERT_TRUE(point.hasValue());
    EXPECT_DOUBLE_EQ(point.value().x(), 1.0);
    EXPECT_DOUBLE_EQ(point.value().y(), 1.0);
}

TEST(CursorNormalizerTest, CenterMapsToHalf) {
    const auto point = CursorNormalizer::normalize(960, 540, 1920, 1080);
    ASSERT_TRUE(point.hasValue());
    EXPECT_DOUBLE_EQ(point.value().x(), 0.5);
    EXPECT_DOUBLE_EQ(point.value().y(), 0.5);
}

TEST(CursorNormalizerTest, RejectsZeroOrNegativeDimensions) {
    EXPECT_EQ(CursorNormalizer::normalize(10, 10, 0, 1080).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(CursorNormalizer::normalize(10, 10, 1920, 0).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(CursorNormalizerTest, ClampsOutOfFrameCoordinates) {
    // Documented behavior: a cursor past the edge (or a negative physical
    // coordinate on a multi-monitor desktop) clamps into [0, 1] rather than
    // being rejected, keeping the stream continuous and schema-valid.
    const auto below = CursorNormalizer::normalize(-50, -50, 1920, 1080);
    ASSERT_TRUE(below.hasValue());
    EXPECT_DOUBLE_EQ(below.value().x(), 0.0);
    EXPECT_DOUBLE_EQ(below.value().y(), 0.0);

    const auto above = CursorNormalizer::normalize(4000, 4000, 1920, 1080);
    ASSERT_TRUE(above.hasValue());
    EXPECT_DOUBLE_EQ(above.value().x(), 1.0);
    EXPECT_DOUBLE_EQ(above.value().y(), 1.0);
}

}  // namespace
