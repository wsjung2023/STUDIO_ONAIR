#include "cursor/windows/WindowsRawInputCursorSource.h"

#include <gtest/gtest.h>

#define NOMINMAX
#include <Windows.h>

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

TEST(WindowsRawInputCursorSourceTest,
     RepeatedRecordingLifetimesDoNotAccumulateProcessHandles) {
    const auto exercise = [](int takes) {
        for (int take = 0; take < takes; ++take) {
            auto source = WindowsRawInputCursorSource::create(
                CursorCaptureGeometry{.left = 0, .top = 0,
                                      .width = 1920, .height = 1080},
                16);
            ASSERT_TRUE(source.hasValue()) << "take " << take << ": "
                                           << source.error().message();
        }
    };

    // User32 and the C++ thread runtime acquire process-lifetime handles on
    // their first hidden-window/thread use. Measure the steady-state take
    // lifecycle after that one-time initialization, not process warm-up.
    exercise(25);
    DWORD handlesBefore = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore));

    exercise(250);

    DWORD handlesAfter = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter));
    EXPECT_LE(handlesAfter, handlesBefore + 4)
        << "Raw Input registration/window/thread handles leaked across takes";
}
