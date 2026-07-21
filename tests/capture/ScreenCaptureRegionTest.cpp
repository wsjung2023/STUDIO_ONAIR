#include "capture/ScreenCaptureRegion.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using creator::capture::cropBgra8;
using creator::capture::encodeRegionTargetId;
using creator::capture::ensureRegionWithinBounds;
using creator::capture::makeScreenCaptureRegion;
using creator::capture::parseRegionTargetId;
using creator::capture::ScreenCaptureRegion;
using creator::core::ErrorCode;

// Builds a tightly packed BGRA8 frame whose pixel at (x,y) encodes its own
// coordinates: B=x, G=y, R=x+y, A=255. That lets a crop assertion prove not just
// the dimensions but that the exact source pixels landed in the region buffer.
std::vector<std::uint8_t> makeCoordinateFrame(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[offset + 0] = static_cast<std::uint8_t>(x);
            pixels[offset + 1] = static_cast<std::uint8_t>(y);
            pixels[offset + 2] = static_cast<std::uint8_t>(x + y);
            pixels[offset + 3] = 255U;
        }
    }
    return pixels;
}

TEST(ScreenCaptureRegionTest, MakeAcceptsPositiveRectangle) {
    const auto region = makeScreenCaptureRegion(10, 20, 640, 480);
    ASSERT_TRUE(region.hasValue());
    EXPECT_EQ(region.value().x, 10u);
    EXPECT_EQ(region.value().y, 20u);
    EXPECT_EQ(region.value().width, 640u);
    EXPECT_EQ(region.value().height, 480u);
}

TEST(ScreenCaptureRegionTest, MakeRejectsEmptyOrNegativeRectangle) {
    for (const auto& bad : std::vector<std::array<std::int64_t, 4>>{
             {0, 0, 0, 100}, {0, 0, 100, 0}, {-1, 0, 100, 100}, {0, -1, 100, 100},
             {0, 0, -5, 100}}) {
        const auto region = makeScreenCaptureRegion(bad[0], bad[1], bad[2], bad[3]);
        ASSERT_FALSE(region.hasValue());
        EXPECT_EQ(region.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(ScreenCaptureRegionTest, BoundsCheckRejectsRegionOutsideMonitor) {
    const auto region = makeScreenCaptureRegion(1900, 0, 100, 100).value();
    // 1900 + 100 = 2000 > 1920 width -> out of bounds.
    const auto outside = ensureRegionWithinBounds(region, 1920, 1080);
    ASSERT_FALSE(outside.hasValue());
    EXPECT_EQ(outside.error().code(), ErrorCode::InvalidArgument);

    const auto inside = ensureRegionWithinBounds(
        makeScreenCaptureRegion(1820, 980, 100, 100).value(), 1920, 1080);
    EXPECT_TRUE(inside.hasValue());
}

TEST(ScreenCaptureRegionTest, TargetIdEncodingRoundTrips) {
    const auto region = makeScreenCaptureRegion(10, 20, 640, 480).value();
    const auto encoded = encodeRegionTargetId("display:42", region);
    const auto parsed = parseRegionTargetId(encoded);
    EXPECT_EQ(parsed.baseId, "display:42");
    ASSERT_TRUE(parsed.region.has_value());
    EXPECT_EQ(*parsed.region, region);
}

TEST(ScreenCaptureRegionTest, PlainTargetIdParsesWithoutRegion) {
    const auto parsed = parseRegionTargetId("display:42");
    EXPECT_EQ(parsed.baseId, "display:42");
    EXPECT_FALSE(parsed.region.has_value());
}

TEST(ScreenCaptureRegionTest, MalformedRegionSuffixIsIgnored) {
    const auto parsed = parseRegionTargetId("display:42#region=10,20,zzz,480");
    // Base id is preserved so the monitor still resolves; the bad region is dropped.
    EXPECT_FALSE(parsed.region.has_value());
}

TEST(ScreenCaptureRegionTest, CropProducesRegionDimensionsAndPixels) {
    const std::uint32_t srcW = 8;
    const std::uint32_t srcH = 6;
    const auto frame = makeCoordinateFrame(srcW, srcH);
    const auto region = makeScreenCaptureRegion(2, 1, 3, 2).value();

    const auto cropped = cropBgra8(frame.data(), srcW, srcH, region);
    ASSERT_TRUE(cropped.hasValue());
    EXPECT_EQ(cropped.value().width, 3u);
    EXPECT_EQ(cropped.value().height, 2u);
    ASSERT_EQ(cropped.value().pixels.size(), static_cast<std::size_t>(3 * 2 * 4));

    // Every cropped pixel must equal the source pixel at (region.x + col,
    // region.y + row): B carries the source x, G carries the source y.
    for (std::uint32_t row = 0; row < 2; ++row) {
        for (std::uint32_t col = 0; col < 3; ++col) {
            const std::size_t offset = (static_cast<std::size_t>(row) * 3 + col) * 4U;
            EXPECT_EQ(cropped.value().pixels[offset + 0],
                      static_cast<std::uint8_t>(region.x + col));
            EXPECT_EQ(cropped.value().pixels[offset + 1],
                      static_cast<std::uint8_t>(region.y + row));
            EXPECT_EQ(cropped.value().pixels[offset + 3], 255U);
        }
    }
}

TEST(ScreenCaptureRegionTest, CropRejectsOutOfBoundsRegionAndNullBuffer) {
    const auto frame = makeCoordinateFrame(4, 4);
    const auto region = makeScreenCaptureRegion(2, 2, 4, 4).value();  // spills past 4x4
    const auto overflow = cropBgra8(frame.data(), 4, 4, region);
    ASSERT_FALSE(overflow.hasValue());
    EXPECT_EQ(overflow.error().code(), ErrorCode::InvalidArgument);

    const auto nullBuffer =
        cropBgra8(nullptr, 4, 4, makeScreenCaptureRegion(0, 0, 2, 2).value());
    ASSERT_FALSE(nullBuffer.hasValue());
    EXPECT_EQ(nullBuffer.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
