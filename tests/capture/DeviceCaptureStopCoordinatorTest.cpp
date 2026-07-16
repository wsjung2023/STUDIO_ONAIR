#include "capture/DeviceCaptureStopCoordinator.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using creator::capture::DeviceCaptureStopCoordinator;
using creator::core::AppError;
using creator::core::ErrorCode;

TEST(DeviceCaptureStopCoordinatorTest, CoalescesAndCompletesEveryCallerExactlyOnce) {
    DeviceCaptureStopCoordinator coordinator;
    int calls = 0;
    coordinator.add([&](auto result) { EXPECT_TRUE(result.hasValue()); ++calls; });
    coordinator.add([&](auto result) { EXPECT_TRUE(result.hasValue()); ++calls; });

    coordinator.finish(creator::core::ok());
    coordinator.finish(AppError{ErrorCode::IoFailure, "late failure"});

    EXPECT_EQ(calls, 2);
}

TEST(DeviceCaptureStopCoordinatorTest, ReplaysFinalFailureToLateCaller) {
    DeviceCaptureStopCoordinator coordinator;
    coordinator.finish(AppError{ErrorCode::IoFailure, "native device stop failed"});
    std::string message;

    coordinator.add([&](auto result) {
        ASSERT_FALSE(result.hasValue());
        message = result.error().message();
    });

    EXPECT_EQ(message, "native device stop failed");
}

}  // namespace
