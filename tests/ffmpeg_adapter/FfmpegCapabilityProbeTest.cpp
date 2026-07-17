#include "ffmpeg_adapter/FfmpegCapabilityProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace {

using creator::ffmpeg_adapter::EncoderCapability;
using creator::ffmpeg_adapter::probeFfmpegCapabilities;

const EncoderCapability* findEncoder(const std::vector<EncoderCapability>& encoders,
                                     std::string_view name) {
    const auto found = std::find_if(encoders.begin(), encoders.end(),
                                    [name](const auto& value) { return value.name == name; });
    return found == encoders.end() ? nullptr : &*found;
}

TEST(FfmpegCapabilityProbeTest, RuntimeIsAuditedDynamicLgplVersion) {
    const auto result = probeFfmpegCapabilities();
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().avcodecVersion >> 16U, 62u);
    EXPECT_EQ(result.value().avformatVersion >> 16U, 62u);
    EXPECT_EQ(result.value().swresampleVersion >> 16U, 6u);
    EXPECT_EQ(result.value().swscaleVersion >> 16U, 9u);
    EXPECT_NE(result.value().license.find("LGPL"), std::string::npos);
    EXPECT_EQ(result.value().configuration.find("--enable-gpl"), std::string::npos);
    EXPECT_EQ(result.value().configuration.find("--enable-nonfree"), std::string::npos);
    EXPECT_NE(result.value().configuration.find("--enable-shared"), std::string::npos);
}

TEST(FfmpegCapabilityProbeTest, RequiredLgplFallbackEncodersExist) {
    const auto result = probeFfmpegCapabilities();
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    const auto* video = findEncoder(result.value().encoders, "mpeg4");
    const auto* audio = findEncoder(result.value().encoders, "aac");
    ASSERT_NE(video, nullptr);
    ASSERT_NE(audio, nullptr);
    EXPECT_TRUE(video->available);
    EXPECT_TRUE(audio->available);
#ifdef _WIN32
    const auto* mediaFoundationAudio =
        findEncoder(result.value().encoders, "aac_mf");
    ASSERT_NE(mediaFoundationAudio, nullptr);
    EXPECT_TRUE(mediaFoundationAudio->available);
#endif
}

}  // namespace
