#include "avatar/AvatarRenderFrame.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using creator::avatar::AvatarRenderFrame;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TEST(AvatarRenderFrameMediaTest, BridgesPackedTransparentPixelsAndKeepsOwnership) {
    auto rendered = AvatarRenderFrame::transparent(TimestampNs{}, 3, 2);
    ASSERT_TRUE(rendered.hasValue()) << rendered.error().message();
    const auto frame = rendered.value().toVideoFrame();
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().pixelFormat, creator::media::PixelFormat::Bgra8);
    EXPECT_EQ(frame.value().visibleRect.width, 3U);
    EXPECT_EQ(frame.value().visibleRect.height, 2U);
    ASSERT_NE(frame.value().platformHandle, nullptr);
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().platformHandle.get());
    EXPECT_EQ(pixels[0], 0U);
    EXPECT_EQ(pixels[23], 0U);
}

TEST(AvatarRenderFrameMediaTest, RejectsPaddedStrideAtMediaBoundary) {
    auto rendered = AvatarRenderFrame::fromBgra(TimestampNs{}, 2, 1, 12,
                                                std::vector<std::uint8_t>(12));
    ASSERT_TRUE(rendered.hasValue());
    const auto frame = rendered.value().toVideoFrame();
    ASSERT_FALSE(frame.hasValue());
    EXPECT_EQ(frame.error().code(), ErrorCode::InvalidState);
}

}  // namespace
