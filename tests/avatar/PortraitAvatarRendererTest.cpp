#include "avatar/PortraitAvatarRenderer.h"

#include "avatar/AvatarParameterMapper.h"
#include "avatar/PlaceholderAvatarRenderer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using creator::avatar::AvatarParameterValue;
using creator::avatar::PortraitAvatarRenderer;
using creator::avatar::PortraitLayers;
using creator::avatar::PortraitRegion;
using Names = creator::avatar::PlaceholderAvatarParameterNames;

constexpr std::uint32_t kSide = 64;

std::vector<std::uint8_t> solid(std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kSide) * kSide * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        bytes[i] = b;
        bytes[i + 1] = g;
        bytes[i + 2] = r;
        bytes[i + 3] = 255;
    }
    return bytes;
}

// neutral = red, blink = blue, speak = green; eyes across the upper third,
// mouth in the lower third.
PortraitLayers layers() {
    return PortraitLayers{.width = kSide,
                          .height = kSide,
                          .neutralBgra = solid(0, 0, 255),
                          .blinkBgra = solid(255, 0, 0),
                          .speakBgra = solid(0, 255, 0),
                          .eyes = PortraitRegion{0.20F, 0.25F, 0.60F, 0.15F},
                          .mouth = PortraitRegion{0.35F, 0.62F, 0.30F, 0.14F}};
}

std::vector<AvatarParameterValue> params(float eyeL, float eyeR, float mouth, float yaw = 0,
                                         float pitch = 0, float roll = 0) {
    return {{Names::kEyeOpenLeft, eyeL},   {Names::kEyeOpenRight, eyeR},
            {Names::kMouthOpen, mouth},    {Names::kHeadYaw, yaw},
            {Names::kHeadPitch, pitch},    {Names::kHeadRoll, roll}};
}

std::array<std::uint8_t, 4> pixel(const creator::avatar::AvatarRenderFrame& frame,
                                  std::uint32_t x, std::uint32_t y) {
    const auto bytes = frame.bytes();
    const std::size_t at = static_cast<std::size_t>(y) * frame.stride() + x * 4U;
    return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
}

// The portrait is drawn centred, filling kPortraitHeightFraction of the frame
// height; outside it the frame is transparent.
TEST(PortraitAvatarRendererTest, DrawsTheNeutralPortraitOnATransparentFrame) {
    auto renderer = PortraitAvatarRenderer::create(layers(), 160, 90);
    ASSERT_TRUE(renderer.hasValue()) << renderer.error().message();
    const auto p = params(1, 1, 0);
    const auto frame = renderer.value()->render(creator::core::TimestampNs{}, p);
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().width(), 160U);
    EXPECT_EQ(frame.value().height(), 90U);
    const auto centre = pixel(frame.value(), 80, 45);
    EXPECT_EQ(centre[3], 255);
    EXPECT_GT(centre[2], 200);  // red
    EXPECT_LT(centre[0], 40);
    EXPECT_EQ(pixel(frame.value(), 2, 2)[3], 0);  // far corner transparent
}

TEST(PortraitAvatarRendererTest, ClosedEyesSwapOnlyTheEyeRegion) {
    auto renderer = PortraitAvatarRenderer::create(layers(), 160, 160).value();
    const auto closed = renderer->render(creator::core::TimestampNs{}, params(0, 0, 0)).value();
    // Portrait side = 160 * fraction, centred; eye region centre is at
    // (0.5, 0.325) of the portrait, mouth at (0.5, 0.69).
    const float side = 160.0F * PortraitAvatarRenderer::kPortraitHeightFraction;
    const float top = (160.0F - side) / 2.0F;
    const auto eyeY = static_cast<std::uint32_t>(top + 0.325F * side);
    const auto chinY = static_cast<std::uint32_t>(top + 0.90F * side);
    const auto eye = pixel(closed, 80, eyeY);
    EXPECT_GT(eye[0], 200) << "eye region shows the blink layer (blue)";
    EXPECT_LT(eye[2], 40);
    const auto chin = pixel(closed, 80, chinY);
    EXPECT_GT(chin[2], 200) << "outside the eye region the neutral layer (red) stays";
}

TEST(PortraitAvatarRendererTest, OnlyTheClosedEyeSwaps) {
    auto renderer = PortraitAvatarRenderer::create(layers(), 160, 160).value();
    const auto leftOnly = renderer->render(creator::core::TimestampNs{}, params(0, 1, 0)).value();
    const float side = 160.0F * PortraitAvatarRenderer::kPortraitHeightFraction;
    const float top = (160.0F - side) / 2.0F;
    const float left = (160.0F - side) / 2.0F;
    const auto eyeY = static_cast<std::uint32_t>(top + 0.325F * side);
    const auto leftEyeX = static_cast<std::uint32_t>(left + 0.35F * side);
    const auto rightEyeX = static_cast<std::uint32_t>(left + 0.65F * side);
    EXPECT_GT(pixel(leftOnly, leftEyeX, eyeY)[0], 200);
    EXPECT_GT(pixel(leftOnly, rightEyeX, eyeY)[2], 200);
}

TEST(PortraitAvatarRendererTest, OpenMouthSwapsOnlyTheMouthRegion) {
    auto renderer = PortraitAvatarRenderer::create(layers(), 160, 160).value();
    const auto talking = renderer->render(creator::core::TimestampNs{}, params(1, 1, 1)).value();
    const float side = 160.0F * PortraitAvatarRenderer::kPortraitHeightFraction;
    const float top = (160.0F - side) / 2.0F;
    const auto mouthY = static_cast<std::uint32_t>(top + 0.69F * side);
    const auto eyeY = static_cast<std::uint32_t>(top + 0.325F * side);
    EXPECT_GT(pixel(talking, 80, mouthY)[1], 200) << "mouth region shows the speak layer (green)";
    EXPECT_GT(pixel(talking, 80, eyeY)[2], 200) << "eyes stay neutral";
    // Below the threshold the mouth stays closed.
    const auto quiet = renderer->render(creator::core::TimestampNs{}, params(1, 1, 0.2F)).value();
    EXPECT_GT(pixel(quiet, 80, mouthY)[2], 200);
}

TEST(PortraitAvatarRendererTest, HeadMotionMovesThePortrait) {
    auto renderer = PortraitAvatarRenderer::create(layers(), 160, 160).value();
    const auto still = renderer->render(creator::core::TimestampNs{}, params(1, 1, 0)).value();
    const auto rolled =
        renderer->render(creator::core::TimestampNs{}, params(1, 1, 0, 0, 0, 1.0F)).value();
    EXPECT_NE(still.bytes().size(), 0U);
    bool differs = false;
    for (std::size_t i = 0; i < still.bytes().size(); ++i) {
        if (still.bytes()[i] != rolled.bytes()[i]) {
            differs = true;
            break;
        }
    }
    EXPECT_TRUE(differs);
}

TEST(PortraitAvatarRendererTest, RejectsMismatchedLayers) {
    auto bad = layers();
    bad.speakBgra.resize(16);
    EXPECT_FALSE(PortraitAvatarRenderer::create(bad, 160, 90).hasValue());
    EXPECT_FALSE(PortraitAvatarRenderer::create(layers(), 0, 90).hasValue());
}

}  // namespace
