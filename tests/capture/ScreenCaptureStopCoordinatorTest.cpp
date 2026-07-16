#include "capture/ScreenCaptureStopCoordinator.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using creator::capture::ScreenCaptureStopCoordinator;
using creator::core::AppError;
using creator::core::ErrorCode;

TEST(ScreenCaptureStopCoordinatorTest, CoalescesAndCompletesEachCallbackOnce) {
    ScreenCaptureStopCoordinator coordinator;
    int firstCalls = 0;
    int secondCalls = 0;
    coordinator.add([&](auto result) {
        EXPECT_TRUE(result.hasValue());
        ++firstCalls;
    });
    coordinator.add([&](auto result) {
        EXPECT_TRUE(result.hasValue());
        ++secondCalls;
    });

    coordinator.finish(creator::core::ok());
    coordinator.finish(AppError{ErrorCode::IoFailure, "late failure"});

    EXPECT_EQ(firstCalls, 1);
    EXPECT_EQ(secondCalls, 1);
}

TEST(ScreenCaptureStopCoordinatorTest, ReplaysFinalErrorToLateCaller) {
    ScreenCaptureStopCoordinator coordinator;
    coordinator.finish(AppError{ErrorCode::IoFailure, "native stop failed"});
    int calls = 0;
    std::string message;

    coordinator.add([&](auto result) {
        ++calls;
        ASSERT_FALSE(result.hasValue());
        message = result.error().message();
    });

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(message, "native stop failed");
}

}  // namespace
