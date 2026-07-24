#include "app/VerticalLayout.h"

#include <gtest/gtest.h>

namespace creator::app {
namespace {

using domain::StudioSourceRole;

TEST(VerticalLayoutTest, ScreenTopCameraBottomOnPortrait) {
    auto s = verticalDefaultTransform(StudioSourceRole::Screen, 1080, 1920);
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->width(), 1.0);  // fit to full portrait width
    EXPECT_LT(s->y(), 0.5);             // screen in the upper half

    auto c = verticalDefaultTransform(StudioSourceRole::Camera, 1080, 1920);
    ASSERT_TRUE(c.has_value());
    EXPECT_GT(c->y(), 0.5);      // camera in the lower half
    EXPECT_GT(c->y(), s->y());   // camera below the screen
}

TEST(VerticalLayoutTest, AvatarIsLowerCornerPipOnPortrait) {
    auto a = verticalDefaultTransform(StudioSourceRole::Avatar, 1080, 1920);
    ASSERT_TRUE(a.has_value());
    EXPECT_LT(a->width(), 1.0);  // small PiP, not full width
    EXPECT_GT(a->x(), 0.5);      // right side
    EXPECT_GT(a->y(), 0.5);      // lower area
    EXPECT_GT(a->zOrder(), 0);   // composited above the video
}

TEST(VerticalLayoutTest, LandscapeReturnsNulloptSoSceneTransformIsKept) {
    EXPECT_FALSE(
        verticalDefaultTransform(StudioSourceRole::Screen, 1920, 1080).has_value());
    EXPECT_FALSE(
        verticalDefaultTransform(StudioSourceRole::Camera, 1920, 1080).has_value());
    // Square is not portrait either.
    EXPECT_FALSE(
        verticalDefaultTransform(StudioSourceRole::Screen, 1080, 1080).has_value());
}

TEST(VerticalLayoutTest, AudioRolesHaveNoVisualTransform) {
    EXPECT_FALSE(verticalDefaultTransform(StudioSourceRole::Microphone, 1080, 1920)
                     .has_value());
    EXPECT_FALSE(verticalDefaultTransform(StudioSourceRole::SystemAudio, 1080, 1920)
                     .has_value());
}

}  // namespace
}  // namespace creator::app
