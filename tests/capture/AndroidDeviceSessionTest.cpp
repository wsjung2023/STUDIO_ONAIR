#include "capture/AndroidDeviceSession.h"

#include <gtest/gtest.h>

namespace {

using creator::capture::AndroidDeviceSession;
using creator::capture::DeviceSessionState;

TEST(AndroidDeviceSessionTest, StaleFailureCannotTerminateNewGeneration) {
    AndroidDeviceSession session;
    const auto stale = session.begin();
    const auto current = session.begin();

    EXPECT_FALSE(session.fail(stale));
    EXPECT_EQ(session.state(), DeviceSessionState::Starting);
    EXPECT_TRUE(session.markStreaming(current));
    EXPECT_TRUE(session.acceptsCallbacks(current));
}

TEST(AndroidDeviceSessionTest, StopBarsCallbacksUntilMatchingNativeRelease) {
    AndroidDeviceSession session;
    const auto generation = session.begin();
    ASSERT_TRUE(session.markStreaming(generation));

    EXPECT_TRUE(session.requestStop(generation));
    EXPECT_EQ(session.state(), DeviceSessionState::Stopping);
    EXPECT_FALSE(session.acceptsCallbacks(generation));
    EXPECT_FALSE(session.markStopped(generation + 1));
    EXPECT_TRUE(session.markStopped(generation));
    EXPECT_EQ(session.state(), DeviceSessionState::Stopped);
}

TEST(AndroidDeviceSessionTest, FailureIsTerminalAndIdempotentUntilRelease) {
    AndroidDeviceSession session;
    const auto generation = session.begin();

    EXPECT_TRUE(session.fail(generation));
    EXPECT_FALSE(session.fail(generation));
    EXPECT_FALSE(session.markStreaming(generation));
    EXPECT_TRUE(session.requestStop(generation));
    EXPECT_TRUE(session.markStopped(generation));
}

}  // namespace
