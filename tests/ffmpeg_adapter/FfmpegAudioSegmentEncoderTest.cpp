#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"

#include "domain/Identifiers.h"
#include "recorder/RecordingTrack.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

namespace fs = std::filesystem;
using creator::core::Result;
using creator::domain::SourceId;
using creator::ffmpeg_adapter::FfmpegAudioSegmentEncoder;
using creator::media::AudioBlock;
using creator::recorder::RecordingTrack;
using creator::recorder::SegmentEncodeConfig;
using creator::recorder::TrackRole;

struct DecodedAudio final {
    int sampleRate{0};
    int channels{0};
    bool decodedFrame{false};
    std::vector<std::int64_t> packetPtsNanoseconds;
};

Result<DecodedAudio> decodeAudio(const fs::path& path) {
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
                                       "could not open encoded audio"};
    }
    const int streamIndex = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1,
                                                nullptr, 0);
    if (streamIndex < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::ParseFailure,
                                       "encoded file has no audio stream"};
    }
    const AVCodec* decoder =
        avcodec_find_decoder(input->streams[streamIndex]->codecpar->codec_id);
    if (decoder == nullptr) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "encoded audio has no decoder"};
    }
    decoderContext = avcodec_alloc_context3(decoder);
    if (decoderContext == nullptr ||
        avcodec_parameters_to_context(decoderContext,
                                      input->streams[streamIndex]->codecpar) < 0 ||
        avcodec_open2(decoderContext, decoder, nullptr) < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "could not open encoded audio decoder"};
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                       "could not allocate audio decode buffers"};
    }

    DecodedAudio result{.sampleRate = decoderContext->sample_rate,
                        .channels = decoderContext->ch_layout.nb_channels};
    constexpr AVRational nanoseconds{1, 1'000'000'000};
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (packet->pts != AV_NOPTS_VALUE) {
                result.packetPtsNanoseconds.push_back(av_rescale_q(
                    packet->pts, input->streams[streamIndex]->time_base, nanoseconds));
            }
            if (avcodec_send_packet(decoderContext, packet) >= 0 &&
                avcodec_receive_frame(decoderContext, frame) >= 0) {
                result.decodedFrame = true;
            }
        }
        av_packet_unref(packet);
    }
    cleanup();
    return result;
}

AudioBlock blockAt(std::uint64_t startFrame, std::uint32_t frames,
                   std::uint32_t channels = 2, std::uint32_t sampleRate = 48'000) {
    auto samples = std::shared_ptr<float[]>(new float[frames * channels]);
    for (std::uint64_t index = 0; index < frames * channels; ++index) {
        samples[index] = static_cast<float>(static_cast<int>(index % 17) - 8) / 32.0F;
    }
    const auto timestamp = std::chrono::nanoseconds{
        static_cast<std::int64_t>((startFrame * 1'000'000'000ULL) / sampleRate)};
    return AudioBlock{.timestamp = creator::core::TimestampNs{timestamp},
                      .sampleRate = sampleRate,
                      .channels = channels,
                      .frameCount = frames,
                      .samples = std::move(samples)};
}

SegmentEncodeConfig config(const fs::path& path) {
    return SegmentEncodeConfig{
        .track = RecordingTrack::create(SourceId::create("microphone-1").value(),
                                        TrackRole::Microphone)
                     .value(),
        .partPath = path,
        .startTime = {},
        .targetDuration = std::chrono::seconds{2},
    };
}

class FfmpegAudioSegmentEncoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = fs::temp_directory_path() / "cs_ffmpeg_audio_test.mka.part";
        std::error_code ignored;
        fs::remove(path_, ignored);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    fs::path path_;
};

TEST_F(FfmpegAudioSegmentEncoderTest, EncodesArbitraryStereoBlocksAndFinalPartialFrame) {
    FfmpegAudioSegmentEncoder encoder;
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(blockAt(0, 127)).hasValue());
    ASSERT_TRUE(encoder.accept(blockAt(127, 511)).hasValue());
    ASSERT_TRUE(encoder.accept(blockAt(638, 2048)).hasValue());

    const auto encoded = encoder.finish(
        creator::core::TimestampNs{std::chrono::milliseconds{60}});

    ASSERT_TRUE(encoded.hasValue()) << encoded.error().message();
    EXPECT_EQ(encoded.value().codecName, "aac");
    const auto decoded = decodeAudio(path_);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(decoded.value().sampleRate, 48'000);
    EXPECT_EQ(decoded.value().channels, 2);
    EXPECT_TRUE(decoded.value().decodedFrame);
}

TEST_F(FfmpegAudioSegmentEncoderTest, EncodesMonoAudio) {
    FfmpegAudioSegmentEncoder encoder;
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(blockAt(0, 1024, 1)).hasValue());
    ASSERT_TRUE(encoder.finish(
                    creator::core::TimestampNs{std::chrono::milliseconds{22}})
                    .hasValue());

    const auto decoded = decodeAudio(path_);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(decoded.value().channels, 1);
    EXPECT_TRUE(decoded.value().decodedFrame);
}

TEST_F(FfmpegAudioSegmentEncoderTest, PreservesAProjectTimestampDiscontinuity) {
    FfmpegAudioSegmentEncoder encoder;
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(blockAt(0, 1024)).hasValue());
    auto afterGap = blockAt(4'800, 1024);
    ASSERT_TRUE(encoder.accept(afterGap).hasValue());
    ASSERT_TRUE(encoder.finish(
                    creator::core::TimestampNs{std::chrono::milliseconds{130}})
                    .hasValue());

    const auto decoded = decodeAudio(path_);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    ASSERT_GE(decoded.value().packetPtsNanoseconds.size(), 2u);
    std::int64_t largestGap = 0;
    for (std::size_t index = 1; index < decoded.value().packetPtsNanoseconds.size(); ++index) {
        largestGap = std::max(largestGap,
                              decoded.value().packetPtsNanoseconds[index] -
                                  decoded.value().packetPtsNanoseconds[index - 1]);
    }
    EXPECT_GT(largestGap, std::chrono::milliseconds{50}.count() * 1'000'000LL);
}

TEST_F(FfmpegAudioSegmentEncoderTest, RejectsFormatChangeAndOverlappingTimestamp) {
    FfmpegAudioSegmentEncoder encoder;
    ASSERT_TRUE(encoder.start(config(path_)).hasValue());
    ASSERT_TRUE(encoder.accept(blockAt(0, 1024)).hasValue());

    const auto changed = encoder.accept(blockAt(1024, 128, 1));
    const auto overlap = encoder.accept(blockAt(100, 128));

    ASSERT_FALSE(changed.hasValue());
    EXPECT_EQ(changed.error().code(), creator::core::ErrorCode::InvalidArgument);
    ASSERT_FALSE(overlap.hasValue());
    EXPECT_EQ(overlap.error().code(), creator::core::ErrorCode::InvalidArgument);
    encoder.abort();
}

}  // namespace
