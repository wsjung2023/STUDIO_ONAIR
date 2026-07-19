#include "capture/AndroidProjectionSession.h"

#include <gtest/gtest.h>

namespace {

using creator::capture::AndroidProjectionSession;
using creator::capture::ProjectionSessionState;

TEST(AndroidProjectionSessionTest, EachApprovedProjectionGetsNewGeneration) {
    AndroidProjectionSession session;
    const auto first = session.beginApprovedProjection();
    EXPECT_EQ(session.state(), ProjectionSessionState::Starting);
    EXPECT_TRUE(session.markStreaming(first));
    EXPECT_EQ(session.state(), ProjectionSessionState::Streaming);

    const auto second = session.beginApprovedProjection();
    EXPECT_GT(second, first);
    EXPECT_EQ(session.state(), ProjectionSessionState::Starting);
}

TEST(AndroidProjectionSessionTest, StaleAndroidCallbackCannotStopNewProjection) {
    AndroidProjectionSession session;
    const auto oldGeneration = session.beginApprovedProjection();
    const auto currentGeneration = session.beginApprovedProjection();
    EXPECT_FALSE(session.onProjectionRevoked(oldGeneration));
    EXPECT_EQ(session.state(), ProjectionSessionState::Starting);
    EXPECT_TRUE(session.onProjectionRevoked(currentGeneration));
    EXPECT_EQ(session.state(), ProjectionSessionState::Revoked);
}

TEST(AndroidProjectionSessionTest, RevocationIsIdempotentForOneGeneration) {
    AndroidProjectionSession session;
    const auto generation = session.beginApprovedProjection();
    EXPECT_TRUE(session.markStreaming(generation));
    EXPECT_TRUE(session.onProjectionRevoked(generation));
    EXPECT_FALSE(session.onProjectionRevoked(generation));
    EXPECT_EQ(session.state(), ProjectionSessionState::Revoked);
}

}  // namespace
