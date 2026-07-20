#include "app/android/AndroidMediaCodecSession.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

namespace {

using creator::app::android::AndroidMediaCodecFormat;
using creator::app::android::AndroidMediaCodecSession;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

TEST(AndroidMediaCodecSessionTest, RejectsStaleGenerationAndTimestampRegression) {
    AndroidMediaCodecSession session;
    const auto first = session.begin(at(100));
    ASSERT_TRUE(first.hasValue());
    const auto format = AndroidMediaCodecFormat::video(1920, 1080);
    ASSERT_TRUE(session.accept(first.value(), at(100), format).hasValue());
    EXPECT_EQ(session.accept(first.value() + 1, at(101), format).error().code(),
              ErrorCode::InvalidState);
    EXPECT_EQ(session.accept(first.value(), at(99), format).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(AndroidMediaCodecSessionTest, PinsFormatAndRequiresInputBeforeFinish) {
    AndroidMediaCodecSession session;
    const auto generation = session.begin(at(100)).value();
    EXPECT_EQ(session.finish(generation, at(200)).error().code(),
              ErrorCode::InvalidState);
    ASSERT_TRUE(session.accept(generation, at(100),
                               AndroidMediaCodecFormat::audio(48'000, 2))
                    .hasValue());
    EXPECT_EQ(session.accept(generation, at(110),
                             AndroidMediaCodecFormat::audio(44'100, 2))
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_TRUE(session.finish(generation, at(200)).hasValue());
}

TEST(AndroidMediaCodecSessionTest, AbortBarsLateInputAndAllowsNewGeneration) {
    AndroidMediaCodecSession session;
    const auto first = session.begin(at(0)).value();
    session.abort(first);
    EXPECT_EQ(session.accept(first, at(0),
                             AndroidMediaCodecFormat::video(1280, 720))
                  .error()
                  .code(),
              ErrorCode::InvalidState);
    const auto second = session.begin(at(10));
    ASSERT_TRUE(second.hasValue());
    EXPECT_GT(second.value(), first);
}

}  // namespace
