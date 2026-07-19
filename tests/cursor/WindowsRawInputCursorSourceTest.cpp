#include "cursor/windows/WindowsRawInputCursorSource.h"

#include <gtest/gtest.h>

using creator::core::ErrorCode;
using creator::cursor::windows::CursorCaptureGeometry;
using creator::cursor::windows::WindowsRawInputCursorSource;

TEST(WindowsRawInputCursorSourceTest, RejectsAnUnboundedQueue) {
    const auto result = WindowsRawInputCursorSource::create(
        CursorCaptureGeometry{.left = 0, .top = 0, .width = 1920, .height = 1080}, 0);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(WindowsRawInputCursorSourceTest, StartsMessageWindowAndInitiallyPollsEmpty) {
    const auto result = WindowsRawInputCursorSource::create(
        CursorCaptureGeometry{.left = 0, .top = 0, .width = 1920, .height = 1080}, 16);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_FALSE(result.value()->poll().has_value());
    EXPECT_FALSE(result.value()->error().has_value());
    EXPECT_EQ(result.value()->droppedSamples(), 0U);
}

