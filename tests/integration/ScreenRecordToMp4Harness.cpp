// Standalone integration harness: drive the REAL Windows screen-capture and
// recording pipeline end to end (no GUI) and produce a finalized H.264 MP4 of
// the primary desktop.
//
// Path used:
//   1. creator::ffmpeg_adapter::windows::makeWindowsCaptureBackend() -> the same
//      FFmpeg gdigrab screen source the app records through.
//   2. Frames are pushed into creator::ffmpeg_adapter::FfmpegVideoSegmentEncoder
//      (the exact encoder the AsyncTrackRecorder drives during a live take). On
//      Windows its default encoder list prefers h264_mf, so the segment carries
//      a real H.264 elementary stream. The segment container is Matroska (as in
//      production).
//   3. The finalized Matroska segment is remuxed (stream copy, no re-encode) into
//      an .mp4 via libavformat so the deliverable is H.264-in-MP4.
//
// This is a driver/harness: it is allowed to sleep for the wall-clock recording
// duration. The pipeline code it drives does not sleep for synchronization.

#include "capture/IVideoFrameSink.h"
#include "core/Result.h"
#include "domain/Identifiers.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"
#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"
#include "recorder/RecordingTrack.h"
#include "recorder/TrackSegmentPorts.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace creator;

// Serializes real capture-thread frame callbacks straight into the production
// video segment encoder, lazily starting the segment on the first frame so the
// segment start matches the first captured presentation timestamp.
class RecordingSink final : public capture::IVideoFrameSink {
public:
    RecordingSink(ffmpeg_adapter::FfmpegVideoSegmentEncoder& encoder,
                  recorder::SegmentEncodeConfig config)
        : encoder_(encoder), config_(std::move(config)) {}

    void onCaptureStarted() noexcept override {}

    void onVideoFrame(media::VideoFrame frame) noexcept override {
        std::lock_guard lock{mutex_};
        if (error_) return;
        if (!started_) {
            config_.startTime = frame.timestamp;
            if (auto started = encoder_.start(config_); !started.hasValue()) {
                error_ = started.error();
                return;
            }
            started_ = true;
            lastTimestamp_ = frame.timestamp;
        } else if (frame.timestamp <= lastTimestamp_) {
            // Keep presentation timestamps strictly increasing for the muxer.
            ++skipped_;
            return;
        }
        const auto timestamp = frame.timestamp;
        if (auto accepted = encoder_.accept(frame); !accepted.hasValue()) {
            error_ = accepted.error();
            return;
        }
        lastTimestamp_ = timestamp;
        ++accepted_;
    }

    void onCaptureError(core::AppError error) noexcept override {
        std::lock_guard lock{mutex_};
        if (!error_) error_ = std::move(error);
    }

    struct Snapshot final {
        bool started{false};
        std::uint64_t accepted{0};
        std::uint64_t skipped{0};
        core::TimestampNs lastTimestamp{};
        std::optional<core::AppError> error;
    };

    Snapshot snapshot() {
        std::lock_guard lock{mutex_};
        return {started_, accepted_, skipped_, lastTimestamp_, error_};
    }

private:
    ffmpeg_adapter::FfmpegVideoSegmentEncoder& encoder_;
    recorder::SegmentEncodeConfig config_;
    std::mutex mutex_;
    bool started_{false};
    std::uint64_t accepted_{0};
    std::uint64_t skipped_{0};
    core::TimestampNs lastTimestamp_{};
    std::optional<core::AppError> error_;
};

std::string avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Container-only remux (stream copy) from the pipeline's Matroska/H.264 segment
// into a fragment-free .mp4. No pixels are re-encoded; the H.264 payload the app
// encoder wrote is preserved bit-for-bit.
int remuxToMp4(const fs::path& input, const fs::path& output) {
    AVFormatContext* in = nullptr;
    AVFormatContext* out = nullptr;
    int rc = avformat_open_input(&in, input.string().c_str(), nullptr, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "remux: open input failed: %s\n", avError(rc).c_str());
        return 1;
    }
    rc = avformat_find_stream_info(in, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "remux: stream info failed: %s\n", avError(rc).c_str());
        avformat_close_input(&in);
        return 1;
    }
    rc = avformat_alloc_output_context2(&out, nullptr, "mp4", output.string().c_str());
    if (rc < 0 || out == nullptr) {
        std::fprintf(stderr, "remux: alloc mp4 output failed: %s\n", avError(rc).c_str());
        avformat_close_input(&in);
        return 1;
    }

    std::vector<int> streamMap(in->nb_streams, -1);
    int outIndex = 0;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* inStream = in->streams[i];
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        AVStream* outStream = avformat_new_stream(out, nullptr);
        if (outStream == nullptr) {
            std::fprintf(stderr, "remux: new stream failed\n");
            avformat_close_input(&in);
            avformat_free_context(out);
            return 1;
        }
        rc = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (rc < 0) {
            std::fprintf(stderr, "remux: copy params failed: %s\n", avError(rc).c_str());
            avformat_close_input(&in);
            avformat_free_context(out);
            return 1;
        }
        outStream->codecpar->codec_tag = 0;
        streamMap[i] = outIndex++;
    }
    if (outIndex == 0) {
        std::fprintf(stderr, "remux: no video stream found in segment\n");
        avformat_close_input(&in);
        avformat_free_context(out);
        return 1;
    }

    if ((out->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = avio_open(&out->pb, output.string().c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            std::fprintf(stderr, "remux: avio_open failed: %s\n", avError(rc).c_str());
            avformat_close_input(&in);
            avformat_free_context(out);
            return 1;
        }
    }
    rc = avformat_write_header(out, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "remux: write header failed: %s\n", avError(rc).c_str());
        if (out->pb) avio_closep(&out->pb);
        avformat_close_input(&in);
        avformat_free_context(out);
        return 1;
    }

    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(in, packet) >= 0) {
        const int mapped = packet->stream_index < static_cast<int>(streamMap.size())
                               ? streamMap[packet->stream_index]
                               : -1;
        if (mapped < 0) {
            av_packet_unref(packet);
            continue;
        }
        AVStream* inStream = in->streams[packet->stream_index];
        AVStream* outStream = out->streams[mapped];
        av_packet_rescale_ts(packet, inStream->time_base, outStream->time_base);
        packet->stream_index = mapped;
        packet->pos = -1;
        rc = av_interleaved_write_frame(out, packet);
        av_packet_unref(packet);
        if (rc < 0) {
            std::fprintf(stderr, "remux: write frame failed: %s\n", avError(rc).c_str());
            av_packet_free(&packet);
            av_write_trailer(out);
            if (out->pb) avio_closep(&out->pb);
            avformat_close_input(&in);
            avformat_free_context(out);
            return 1;
        }
    }
    av_packet_free(&packet);
    rc = av_write_trailer(out);
    if (rc < 0) {
        std::fprintf(stderr, "remux: write trailer failed: %s\n", avError(rc).c_str());
    }
    if (out->pb) avio_closep(&out->pb);
    avformat_close_input(&in);
    avformat_free_context(out);
    return rc < 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path outputDir =
        argc > 1 ? fs::path{argv[1]} : fs::current_path() / "win-capture-demo";
    const int recordSeconds = argc > 2 ? std::atoi(argv[2]) : 5;

    std::error_code ec;
    fs::create_directories(outputDir, ec);
    const fs::path segmentPath = outputDir / "screen_segment.mkv";
    const fs::path mp4Path = outputDir / "screen.mp4";
    fs::remove(segmentPath, ec);
    fs::remove(mp4Path, ec);

    std::printf("[harness] output dir : %s\n", outputDir.string().c_str());
    std::printf("[harness] record secs: %d\n", recordSeconds);
    std::fflush(stdout);

    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    if (!backend.screenDiscovery || !backend.screenSourceFactory) {
        std::fprintf(stderr, "[harness] FATAL: capture backend is incomplete\n");
        return 2;
    }

    std::promise<core::Result<std::vector<capture::ScreenCaptureTarget>>> promise;
    auto future = promise.get_future();
    backend.screenDiscovery->enumerate(
        [&promise](auto result) { promise.set_value(std::move(result)); });
    if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
        std::fprintf(stderr, "[harness] FATAL: display enumeration timed out\n");
        return 2;
    }
    auto displays = future.get();
    if (!displays.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: enumerate displays: %s\n",
                     displays.error().message().c_str());
        return 2;
    }
    if (displays.value().empty()) {
        std::fprintf(stderr, "[harness] FATAL: no displays found\n");
        return 2;
    }
    const auto& display = displays.value().front();
    std::printf("[harness] display     : %s (%ux%u)\n",
                display.displayName().c_str(), display.width(), display.height());
    std::fflush(stdout);

    auto track = recorder::RecordingTrack::create(
        domain::SourceId::create("windows/screen").value(),
        recorder::TrackRole::Screen);
    if (!track.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: track create: %s\n",
                     track.error().message().c_str());
        return 2;
    }

    // Default VideoEncoderOptions -> Windows encoder preference {h264_mf, ...}.
    ffmpeg_adapter::FfmpegVideoSegmentEncoder encoder{
        std::make_unique<ffmpeg_adapter::CpuBgraFrameMapper>(),
        ffmpeg_adapter::VideoEncoderOptions{}};

    recorder::SegmentEncodeConfig config{
        .track = std::move(track).value(),
        .partPath = segmentPath,
        .startTime = {},
        .targetDuration = std::chrono::seconds{recordSeconds},
    };

    auto sink = std::make_shared<RecordingSink>(encoder, std::move(config));

    auto source = backend.screenSourceFactory->create(display.id(), sink);
    if (!source.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: create screen source: %s\n",
                     source.error().message().c_str());
        return 2;
    }
    if (auto started = source.value()->start({.targetWidth = display.width(),
                                              .targetHeight = display.height(),
                                              .frameRateNumerator = 30,
                                              .frameRateDenominator = 1});
        !started.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: start capture: %s\n",
                     started.error().message().c_str());
        return 2;
    }
    std::printf("[harness] capturing gdigrab desktop for %d s...\n", recordSeconds);
    std::fflush(stdout);

    std::this_thread::sleep_for(std::chrono::seconds{recordSeconds});

    // Clean stop: the async barrier bars further frame callbacks before we finish.
    std::promise<core::Result<void>> stopPromise;
    auto stopFuture = stopPromise.get_future();
    source.value()->stopAsync(
        [&stopPromise](auto result) { stopPromise.set_value(std::move(result)); });
    if (stopFuture.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
        std::fprintf(stderr, "[harness] FATAL: capture stop timed out\n");
        return 2;
    }
    if (auto stopped = stopFuture.get(); !stopped.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: capture stop: %s\n",
                     stopped.error().message().c_str());
        return 2;
    }

    const auto snap = sink->snapshot();
    std::printf("[harness] frames accepted=%llu skipped=%llu\n",
                static_cast<unsigned long long>(snap.accepted),
                static_cast<unsigned long long>(snap.skipped));
    std::fflush(stdout);
    if (snap.error) {
        std::fprintf(stderr, "[harness] FATAL: capture/encode error: %s\n",
                     snap.error->message().c_str());
        return 3;
    }
    if (!snap.started || snap.accepted == 0) {
        std::fprintf(stderr, "[harness] FATAL: no frames were encoded\n");
        return 3;
    }

    // Finalize the segment (flush encoder, write Matroska trailer).
    auto finished = encoder.finish(snap.lastTimestamp);
    if (!finished.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: finish segment: %s\n",
                     finished.error().message().c_str());
        return 3;
    }
    std::printf("[harness] segment codec=%s bytes=%llu -> %s\n",
                finished.value().codecName.c_str(),
                static_cast<unsigned long long>(finished.value().bytesWritten),
                segmentPath.string().c_str());
    std::fflush(stdout);

    // Container finalize: stream-copy Matroska/H.264 into MP4.
    if (const int rc = remuxToMp4(segmentPath, mp4Path); rc != 0) {
        std::fprintf(stderr, "[harness] FATAL: remux to mp4 failed\n");
        return 4;
    }
    const auto mp4Bytes = fs::file_size(mp4Path, ec);
    std::printf("[harness] OK mp4=%s bytes=%llu\n", mp4Path.string().c_str(),
                static_cast<unsigned long long>(mp4Bytes));
    std::printf("[harness] segment_codec=%s frames=%llu\n",
                finished.value().codecName.c_str(),
                static_cast<unsigned long long>(snap.accepted));
    return 0;
}
