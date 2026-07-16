#include "app/MultiTrackRecordingService.h"
#include "app/ProjectSegmentLifecycleSink.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"
#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"
#include "project_store/ProjectPackageStore.h"
#include "recorder/AsyncTrackRecorder.h"
#include "recorder/DiskSpaceMonitor.h"
#include "recorder/DurableSegmentPublisher.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;
using creator::app::MultiTrackRecordingService;
using creator::app::MultiTrackRecordingSummary;
using creator::app::ProjectSegmentLifecycleContext;
using creator::app::ProjectSegmentLifecycleSink;
using creator::core::Result;
using creator::domain::RecordingSession;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::ffmpeg_adapter::AudioEncoderOptions;
using creator::ffmpeg_adapter::CpuBgraFrameBuffer;
using creator::ffmpeg_adapter::CpuBgraFrameMapper;
using creator::ffmpeg_adapter::FfmpegAudioSegmentEncoder;
using creator::ffmpeg_adapter::FfmpegVideoSegmentEncoder;
using creator::ffmpeg_adapter::VideoEncoderOptions;
using creator::project_store::ProjectPackageStore;
using creator::recorder::AsyncTrackRecorder;
using creator::recorder::AsyncTrackRecorderConfig;
using creator::recorder::DiskSpaceMonitor;
using creator::recorder::DurableSegmentPublisher;
using creator::recorder::RecordingTrack;
using creator::recorder::TrackRole;

creator::core::Utc utc(std::string_view value) {
    return creator::core::Utc::parseRfc3339(value).value();
}

Result<void> decodeAtLeastOneFrame(const fs::path& path) {
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
        return creator::core::AppError{creator::core::ErrorCode::ParseFailure,
                                       "Could not open a published recording segment"};
    }
    const auto mediaType = path.extension() == ".mka" ? AVMEDIA_TYPE_AUDIO
                                                       : AVMEDIA_TYPE_VIDEO;
    const int streamIndex = av_find_best_stream(input, mediaType, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::ParseFailure,
                                       "Published segment has no expected media stream"};
    }
    const AVCodec* decoder =
        avcodec_find_decoder(input->streams[streamIndex]->codecpar->codec_id);
    if (decoder == nullptr) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "Published segment codec cannot be decoded"};
    }
    decoderContext = avcodec_alloc_context3(decoder);
    if (decoderContext == nullptr ||
        avcodec_parameters_to_context(decoderContext,
                                      input->streams[streamIndex]->codecpar) < 0 ||
        avcodec_open2(decoderContext, decoder, nullptr) < 0) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::ParseFailure,
                                       "Could not initialize segment decoder"};
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        cleanup();
        return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                       "Could not allocate segment decode buffers"};
    }
    bool decoded = false;
    while (!decoded && av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == streamIndex &&
            avcodec_send_packet(decoderContext, packet) >= 0 &&
            avcodec_receive_frame(decoderContext, frame) >= 0) {
            decoded = true;
        }
        av_packet_unref(packet);
    }
    cleanup();
    if (!decoded) {
        return creator::core::AppError{creator::core::ErrorCode::ParseFailure,
                                       "Published segment did not decode a frame"};
    }
    return creator::core::ok();
}

std::unique_ptr<AsyncTrackRecorder> makeRecorder(
    const RecordingTrack& track, const fs::path& packagePath,
    std::shared_ptr<ProjectSegmentLifecycleContext> context) {
    std::unique_ptr<creator::recorder::ITrackSegmentEncoder> encoder;
    if (track.mediaKind() == creator::recorder::TrackMediaKind::Video) {
        encoder = std::make_unique<FfmpegVideoSegmentEncoder>(
            std::make_unique<CpuBgraFrameMapper>(),
            VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"},
                                .frameRateNumerator = 30,
                                .frameRateDenominator = 1,
                                .bitRate = 500'000});
    } else {
        encoder = std::make_unique<FfmpegAudioSegmentEncoder>(
            AudioEncoderOptions{.bitRate = 96'000});
    }
    auto publisher = std::make_unique<DurableSegmentPublisher>(
        packagePath, creator::recorder::makeSegmentFileOperations(packagePath),
        std::make_unique<ProjectSegmentLifecycleSink>(std::move(context)));
    AsyncTrackRecorderConfig config{
        .track = track,
        .packageRoot = packagePath,
        .recordingStartTime = {},
        .segmentDuration = std::chrono::seconds{2},
        .videoQueueCapacity = 160,
        .audioQueueFrameCapacity = 240'000,
        .nextSegmentEstimateBytes = 1,
    };
    return std::make_unique<AsyncTrackRecorder>(
        std::move(config), std::move(encoder), std::move(publisher),
        std::make_unique<DiskSpaceMonitor>(
            std::make_unique<creator::recorder::FilesystemDiskSpaceProbe>(), 0));
}

creator::media::VideoFrame videoFrame(std::uint32_t index) {
    auto buffer = CpuBgraFrameBuffer::create(64, 48).value();
    std::fill_n(buffer->data(), buffer->size(),
                static_cast<std::uint8_t>((index * 3U) % 255U));
    const auto timestamp = std::chrono::nanoseconds{
        static_cast<std::int64_t>((static_cast<std::uint64_t>(index) *
                                   1'000'000'000ULL) /
                                  30ULL)};
    return creator::media::VideoFrame{
        .timestamp = creator::core::TimestampNs{timestamp},
        .width = 64,
        .height = 48,
        .visibleRect = {0, 0, 64, 48},
        .contentWidth = 64,
        .contentHeight = 48,
        .pixelFormat = creator::media::PixelFormat::Bgra8,
        .platformHandle = std::move(buffer),
    };
}

creator::media::AudioBlock audioBlock(std::uint32_t index) {
    constexpr std::uint32_t frames = 480;
    auto samples = std::shared_ptr<float[]>(new float[frames * 2]);
    for (std::uint32_t sample = 0; sample < frames * 2; ++sample) {
        samples[sample] = static_cast<float>(static_cast<int>(sample % 13) - 6) / 32.0F;
    }
    return creator::media::AudioBlock{
        .timestamp = creator::core::TimestampNs{std::chrono::milliseconds{index * 10ULL}},
        .sampleRate = 48'000,
        .channels = 2,
        .frameCount = frames,
        .samples = std::move(samples),
    };
}

TEST(RecordingPipelineIntegrationTest, FilesReadyRowsAndDecodedStreamsAgree) {
    const auto root = fs::temp_directory_path() / "cs_recording_pipeline_integration";
    const auto packagePath = root / "take.cstudio";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);

    auto store = std::make_shared<ProjectPackageStore>();
    auto created = store->create(packagePath, "Pipeline integration");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    const auto sessionId = SessionId::create("session-pipeline").value();
    ASSERT_TRUE(store->beginRecording(packagePath, sessionId, {},
                                      utc("2026-07-16T10:00:00Z"))
                    .hasValue());
    RecordingSession session{sessionId};
    ASSERT_TRUE(session.start({}).hasValue());
    auto contextResult = ProjectSegmentLifecycleContext::create(
        store, packagePath, std::move(session));
    ASSERT_TRUE(contextResult.hasValue()) << contextResult.error().message();
    auto context = std::move(contextResult).value();
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(makeRecorder(
                    RecordingTrack::create(screen, TrackRole::Screen).value(),
                    packagePath, context))
                    .hasValue());
    ASSERT_TRUE(service.addTrack(makeRecorder(
                    RecordingTrack::create(microphone, TrackRole::Microphone).value(),
                    packagePath, context))
                    .hasValue());
    ASSERT_TRUE(service.start().hasValue());

    for (std::uint32_t frame = 0; frame < 120; ++frame) {
        ASSERT_TRUE(service.accept(screen, videoFrame(frame)).hasValue());
    }
    for (std::uint32_t block = 0; block < 400; ++block) {
        ASSERT_TRUE(service.accept(microphone, audioBlock(block)).hasValue());
    }
    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    service.stopAsync(creator::core::TimestampNs{std::chrono::seconds{4}},
                      [promise](const auto& result) { promise->set_value(result); });
    const auto stopped = future.get();
    ASSERT_TRUE(stopped.hasValue()) << stopped.error().message();
    ASSERT_EQ(stopped.value().tracks.size(), 2u);

    const auto sessionSnapshot = context->sessionSnapshot();
    ASSERT_EQ(sessionSnapshot.segmentCount(), 4u);
    std::set<std::string> indexedPaths;
    for (const auto& segment : sessionSnapshot.segments()) {
        EXPECT_EQ(segment.status, creator::domain::SegmentStatus::Ready);
        indexedPaths.insert(segment.relativePath);
        const auto finalPath = packagePath / fs::path{segment.relativePath};
        ASSERT_TRUE(fs::is_regular_file(finalPath));
        const auto decoded = decodeAtLeastOneFrame(finalPath);
        ASSERT_TRUE(decoded.hasValue()) << finalPath << ": " << decoded.error().message();
    }

    std::set<std::string> mediaPaths;
    for (fs::recursive_directory_iterator iterator{packagePath};
         iterator != fs::recursive_directory_iterator{}; ++iterator) {
        if (iterator->is_regular_file() &&
            (iterator->path().extension() == ".mkv" ||
             iterator->path().extension() == ".mka")) {
            mediaPaths.insert(fs::relative(iterator->path(), packagePath).generic_string());
        }
    }
    EXPECT_EQ(mediaPaths, indexedPaths);
    const auto completed = context->complete(
        creator::core::TimestampNs{std::chrono::seconds{4}},
        utc("2026-07-16T10:00:04Z"));
    ASSERT_TRUE(completed.hasValue()) << completed.error().message();
    EXPECT_EQ(completed.value().segmentCount(), 4u);

    context.reset();
    store.reset();
    fs::remove_all(root, ignored);
}

}  // namespace
