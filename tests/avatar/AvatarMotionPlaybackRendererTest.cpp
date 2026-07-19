#include "avatar/AvatarMotionPlaybackRenderer.h"
#include "avatar/AvatarMotionSerializer.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

using creator::avatar::AvatarMotionPlayback;
using creator::avatar::AvatarMotionPlaybackRenderer;
using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarMotionSerializer;
using creator::avatar::AvatarParameterMapper;
using creator::avatar::AvatarParameterSource;
using creator::avatar::AvatarRenderFrame;
using creator::avatar::AvatarRenderPipeline;
using creator::avatar::AvatarProviderId;
using creator::avatar::ExpressionParameters;
using creator::avatar::IAvatarRenderer;
using creator::core::DurationNs;
using creator::core::Result;
using creator::core::TimestampNs;

class TransparentRenderer final : public IAvatarRenderer {
public:
    Result<AvatarRenderFrame> render(
        TimestampNs timestamp,
        std::span<const creator::avatar::AvatarParameterValue>) override {
        return AvatarRenderFrame::transparent(timestamp, 2, 2);
    }
};

TEST(AvatarMotionPlaybackRendererTest, ProducesTimestampedMediaFrameAtEditorTime) {
    const auto path = std::filesystem::temp_directory_path() /
                      "avatar-playback-renderer.ndjson";
    const auto provider = AvatarProviderId::create("fake").value();
    const AvatarMotionSample sample{TimestampNs{}, ExpressionParameters{}, provider};
    {
        std::ofstream output{path};
        output << AvatarMotionSerializer{}.toNdjsonLine(sample);
    }
    const auto playback = AvatarMotionPlayback::load(path);
    ASSERT_TRUE(playback.hasValue()) << playback.error().message();
    const auto mapper = AvatarParameterMapper::create({
        {"Mouth", AvatarParameterSource::MouthOpen}});
    ASSERT_TRUE(mapper.hasValue());
    TransparentRenderer renderer;
    AvatarRenderPipeline renderPipeline{mapper.value(), renderer};
    AvatarMotionPlaybackRenderer playbackRenderer{playback.value(), renderPipeline};

    const auto frame = playbackRenderer.renderAt(TimestampNs{DurationNs{10}});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().timestamp, TimestampNs{});
    EXPECT_EQ(frame.value().width, 2U);
    EXPECT_EQ(frame.value().height, 2U);
    EXPECT_EQ(frame.value().pixelFormat, creator::media::PixelFormat::Bgra8);
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace
