#include "avatar/AvatarMotionPlayback.h"
#include "avatar/AvatarMotionSerializer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

using creator::avatar::AvatarMotionPlayback;
using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarMotionSerializer;
using creator::avatar::AvatarProviderId;
using creator::avatar::ExpressionParameters;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TEST(AvatarMotionPlaybackTest, LoadsAndInterpolatesRecordedMotion) {
    const auto path = std::filesystem::temp_directory_path() / "avatar-playback.ndjson";
    const auto provider = AvatarProviderId::create("fake").value();
    const AvatarMotionSample first{TimestampNs{}, ExpressionParameters{.mouthOpen = 0.0F}, provider};
    const AvatarMotionSample second{TimestampNs{DurationNs{1'000'000'000}},
                                    ExpressionParameters{.mouthOpen = 1.0F}, provider};
    {
        std::ofstream output{path};
        AvatarMotionSerializer serializer;
        output << serializer.toNdjsonLine(first) << serializer.toNdjsonLine(second);
    }
    const auto playback = AvatarMotionPlayback::load(path);
    ASSERT_TRUE(playback.hasValue()) << playback.error().message();
    const auto middle = playback.value().sampleAt(
        TimestampNs{DurationNs{500'000'000}});
    ASSERT_TRUE(middle.hasValue()) << middle.error().message();
    EXPECT_FLOAT_EQ(middle.value().parameters.mouthOpen, 0.5F);
    EXPECT_EQ(middle.value().timestamp, TimestampNs{DurationNs{500'000'000}});
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST(AvatarMotionPlaybackTest, ClampsOutsideRangeAndRejectsUnorderedInput) {
    const auto path = std::filesystem::temp_directory_path() / "avatar-playback-invalid.ndjson";
    const auto provider = AvatarProviderId::create("fake").value();
    AvatarMotionSerializer serializer;
    const AvatarMotionSample first{TimestampNs{DurationNs{1}}, ExpressionParameters{}, provider};
    const AvatarMotionSample second{TimestampNs{}, ExpressionParameters{}, provider};
    { std::ofstream output{path}; output << serializer.toNdjsonLine(first) << serializer.toNdjsonLine(second); }
    const auto invalid = AvatarMotionPlayback::load(path);
    ASSERT_FALSE(invalid.hasValue());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace
