#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"

#include "domain/Identifiers.h"
#include "recorder/RecordingTrack.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using creator::core::Result;
using creator::domain::SourceId;
using creator::ffmpeg_adapter::FfmpegVideoSegmentEncoder;
using creator::ffmpeg_adapter::VideoEncoderOptions;
using creator::media::VideoFrame;
using creator::recorder::IVideoFrameMapper;
using creator::recorder::MappedVideoFrame;
using creator::recorder::RecordingTrack;
using creator::recorder::SegmentEncodeConfig;
using creator::recorder::TrackRole;

struct DecodedVideo final {
    bool firstPacketIsKey{false};
    bool decodedFrame{false};
};

Result<DecodedVideo> decodeVideo(const fs::path& path) {
    AVFormatContext* input = nullptr;
    AVCodecContext* decoderContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    const auto cleanup = [&] {
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (decoderContext != nullptr) avcodec_free_context(&decoderContext);
        if (input != nullptr) avformat_close_input(&input);
    };
    const auto pathText = path.string();
    if (avformat_open_input(&input, pathText.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(input, nullptr) < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                       "could not open encoded video"};
    }
    const int streamIndex = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1,
                                                nullptr, 0);
    if (streamIndex < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::ParseFailure,
                                       "encoded file has no video stream"};
    }
    const AVCodec* decoder =
        avcodec_find_decoder(input->streams[streamIndex]->codecpar->codec_id);
    decoderContext = avcodec_alloc_context3(decoder);
    if (decoder == nullptr || decoderContext == nullptr ||
        avcodec_parameters_to_context(decoderContext,
                                      input->streams[streamIndex]->codecpar) < 0 ||
        avcodec_open2(decoderContext, decoder, nullptr) < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "could not open encoded video decoder"};
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                       "could not allocate video decode buffers"};
    }

    DecodedVideo result;
    bool sawPacket = false;
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (!sawPacket) {
                result.firstPacketIsKey = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                sawPacket = true;
            }
            if (avcodec_send_packet(decoderContext, packet) >= 0 &&
                avcodec_receive_frame(decoderContext, frame) >= 0) {
                result.decodedFrame = true;
            }
        }
        av_packet_unref(packet);
        if (result.decodedFrame) break;
    }
    cleanup();
    return result;
}

struct Pixels final {
    std::vector<std::uint8_t> bytes;
};

class Mapper final : public IVideoFrameMapper {
public:
    Result<MappedVideoFrame> map(const VideoFrame& frame) override {
        if (fail) {
            return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "mapping failed"};
        }
        auto pixels = std::static_pointer_cast<Pixels>(frame.platformHandle);
        return MappedVideoFrame{.timestamp = frame.timestamp,
                                .width = frame.width,
                                .height = frame.height,
                                .rowBytes = frame.width * 4ULL,
                                .data = pixels->bytes.data(),
                                .owner = std::move(pixels)};
    }

    bool fail{false};
};

VideoFrame frameAt(std::chrono::milliseconds timestamp, std::uint8_t value) {
    auto pixels = std::make_shared<Pixels>();
    pixels->bytes.resize(64 * 48 * 4, value);
    return VideoFrame{.timestamp = creator::core::TimestampNs{timestamp},
                      .width = 64,
                      .height = 48,
                      .visibleRect = {0, 0, 64, 48},
                      .contentWidth = 64,
                      .contentHeight = 48,
                      .pixelFormat = creator::media::PixelFormat::Bgra8,
                      .platformHandle = std::move(pixels)};
}

SegmentEncodeConfig config(const fs::path& path) {
    return SegmentEncodeConfig{
        .track = RecordingTrack::create(SourceId::create("screen-1").value(),
                                        TrackRole::Screen)
                     .value(),
        .partPath = path,
        .startTime = {},
        .targetDuration = std::chrono::seconds{2},
    };
}

class FfmpegVideoSegmentEncoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = fs::temp_directory_path() / "cs_ffmpeg_video_test.mkv.part";
        std::error_code ignored;
        fs::remove(path_, ignored);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    fs::path path_;
};

TEST_F(FfmpegVideoSegmentEncoderTest, EncodesStandaloneMpeg4MatroskaSegment) {
    auto mapper = std::make_unique<Mapper>();
    FfmpegVideoSegmentEncoder encoder{
        std::move(mapper), VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"}}};
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{0}, 20)).hasValue());
    ASSERT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{33}, 80)).hasValue());
    ASSERT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{66}, 140)).hasValue());

    const auto result = encoder.finish(
        creator::core::TimestampNs{std::chrono::milliseconds{100}});

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().codecName, "mpeg4");
    EXPECT_GT(result.value().bytesWritten, 0u);
    EXPECT_TRUE(fs::is_regular_file(path_));
    const auto decoded = decodeVideo(path_);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_TRUE(decoded.value().firstPacketIsKey);
    EXPECT_TRUE(decoded.value().decodedFrame);
}

TEST_F(FfmpegVideoSegmentEncoderTest, MapperFailurePropagatesWithoutPublishingBytes) {
    auto mapper = std::make_unique<Mapper>();
    mapper->fail = true;
    FfmpegVideoSegmentEncoder encoder{
        std::move(mapper), VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"}}};
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());

    const auto result = encoder.accept(frameAt(std::chrono::milliseconds{0}, 20));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message(), "mapping failed");
    encoder.abort();
    EXPECT_FALSE(fs::exists(path_));
}

TEST_F(FfmpegVideoSegmentEncoderTest, RejectsDimensionChangeWithinSegment) {
    FfmpegVideoSegmentEncoder encoder{
        std::make_unique<Mapper>(),
        VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"}}};
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{0}, 20)).hasValue());
    auto changed = frameAt(std::chrono::milliseconds{33}, 80);
    changed.width = 32;

    const auto result = encoder.accept(changed);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
    encoder.abort();
}

TEST_F(FfmpegVideoSegmentEncoderTest, ReusesEncoderForFollowingSegment) {
    FfmpegVideoSegmentEncoder encoder{
        std::make_unique<Mapper>(),
        VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"}}};
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{0}, 20)).hasValue());
    const auto first = encoder.finish(
        creator::core::TimestampNs{std::chrono::milliseconds{33}});
    ASSERT_TRUE(first.hasValue()) << first.error().message();
    std::error_code ignored;
    fs::remove(path_, ignored);

    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{0}, 40)).hasValue());
    const auto second = encoder.finish(
        creator::core::TimestampNs{std::chrono::milliseconds{33}});
    EXPECT_TRUE(second.hasValue()) << second.error().message();
}

}  // namespace
