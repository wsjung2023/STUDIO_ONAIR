// Standalone integration harness: drive the REAL Windows screen + audio capture
// pipeline end to end (no GUI) and produce a finalized MP4 that carries BOTH the
// H.264 desktop video AND a non-silent AAC audio stream.
//
// Paths used (all the same code the app records through):
//   1. Screen  : creator::ffmpeg_adapter::windows screen source (FFmpeg gdigrab)
//                -> FfmpegVideoSegmentEncoder -> Matroska/H.264 segment.
//   2. System  : the Windows device backend's SystemAudioSource (native WASAPI
//                loopback) -> IAudioBlockSink.
//   3. Mic     : the Windows device backend's MicrophoneSource (FFmpeg dshow)
//                -> IAudioBlockSink.
//   4. The two audio sources are resampled to a common 48 kHz stereo project
//      rate, summed, and fed to FfmpegAudioSegmentEncoder (the exact AAC encoder
//      AsyncTrackRecorder drives) -> Matroska/AAC segment.
//   5. Both finalized segments are remuxed (stream copy, no re-encode) into one
//      screen_with_audio.mp4 via libavformat.
//
// To guarantee the loopback stream is non-silent without a human in the loop the
// harness plays a generated tone WAV through the default render endpoint for the
// duration of the take (PlaySound, winmm). If -- and only if -- both real audio
// sources come back silent, it falls back to a synthesized tone injected into the
// audio-encode path and says so loudly. Nothing is faked silently.
//
// This is a driver/harness: it is allowed to sleep for the wall-clock recording
// duration. The pipeline code it drives does not sleep for synchronization.

#define _USE_MATH_DEFINES

#include "capture/DeviceCaptureTypes.h"
#include "capture/IAudioBlockSink.h"
#include "capture/IDeviceCaptureBackend.h"
#include "capture/IDeviceCaptureSource.h"
#include "capture/IVideoFrameSink.h"
#include "core/Result.h"
#include "domain/Identifiers.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"
#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"
#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"
#include "media/MediaTypes.h"
#include "recorder/RecordingTrack.h"
#include "recorder/TrackSegmentPorts.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace creator;

constexpr std::uint32_t kProjectRate = 48'000;
constexpr std::uint32_t kProjectChannels = 2;

std::string avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// ---------------------------------------------------------------------------
// Video: serialize real capture-thread frames straight into the production video
// segment encoder (identical to ScreenRecordToMp4Harness).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Audio: collect real AudioBlocks from one source, resample to the common
// 48 kHz stereo project rate, and buffer them for offline mixing. Buffering the
// whole ~5 s take (a few MB) removes all cross-source concurrency from the mix
// stage; the capture threads only ever append.
// ---------------------------------------------------------------------------
struct SwrDeleter final {
    void operator()(SwrContext* value) const noexcept { swr_free(&value); }
};

class CollectingAudioSink final : public capture::IAudioBlockSink {
public:
    explicit CollectingAudioSink(std::string label) : label_(std::move(label)) {}

    void onCaptureStarted() noexcept override {}

    void onAudioBlock(media::AudioBlock block) noexcept override {
        std::lock_guard lock{mutex_};
        if (block.frameCount == 0 || block.sampleRate == 0 || block.channels == 0 ||
            !block.samples) {
            return;
        }
        if (!firstArrival_) {
            firstArrival_ = std::chrono::steady_clock::now();
        }
        if (!resampler_ || block.sampleRate != inRate_ || block.channels != inChannels_) {
            if (!buildResampler(block.sampleRate, block.channels)) return;
        }
        const int maxOut = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler_.get(), inRate_) + block.frameCount, kProjectRate,
            inRate_, AV_ROUND_UP));
        if (maxOut <= 0) return;
        std::vector<float> out(static_cast<std::size_t>(maxOut) * kProjectChannels);
        std::uint8_t* outPtr[]{reinterpret_cast<std::uint8_t*>(out.data())};
        const auto* inPtr =
            reinterpret_cast<const std::uint8_t*>(block.samples.get());
        const int converted = swr_convert(resampler_.get(), outPtr, maxOut, &inPtr,
                                          static_cast<int>(block.frameCount));
        if (converted <= 0) return;
        const std::size_t produced =
            static_cast<std::size_t>(converted) * kProjectChannels;
        for (std::size_t i = 0; i < produced; ++i) {
            const float v = out[i];
            sumSquares_ += static_cast<double>(v) * v;
            peak_ = std::max(peak_, std::fabs(v));
        }
        samples_.insert(samples_.end(), out.begin(), out.begin() + produced);
        ++blocks_;
    }

    void onCaptureError(core::AppError error) noexcept override {
        std::lock_guard lock{mutex_};
        if (!error_) error_ = std::move(error);
    }

    struct Result final {
        std::string label;
        std::vector<float> samples;  // interleaved 48k stereo
        std::optional<std::chrono::steady_clock::time_point> firstArrival;
        std::uint64_t blocks{0};
        double rmsDbfs{-120.0};
        double peakDbfs{-120.0};
        std::optional<core::AppError> error;
    };

    Result take() {
        std::lock_guard lock{mutex_};
        const std::size_t frames = samples_.size() / kProjectChannels;
        double rms = -120.0;
        double peak = -120.0;
        if (frames > 0) {
            const double meanSq =
                sumSquares_ / static_cast<double>(samples_.size());
            rms = meanSq > 0 ? 20.0 * std::log10(std::sqrt(meanSq)) : -120.0;
            peak = peak_ > 0 ? 20.0 * std::log10(peak_) : -120.0;
        }
        return Result{label_,      std::move(samples_), firstArrival_, blocks_,
                      rms,         peak,                error_};
    }

private:
    bool buildResampler(std::uint32_t rate, std::uint32_t channels) {
        AVChannelLayout inLayout{};
        av_channel_layout_default(&inLayout, static_cast<int>(channels));
        AVChannelLayout outLayout{};
        av_channel_layout_default(&outLayout, static_cast<int>(kProjectChannels));
        SwrContext* raw = nullptr;
        const int alloc = swr_alloc_set_opts2(
            &raw, &outLayout, AV_SAMPLE_FMT_FLT, static_cast<int>(kProjectRate),
            &inLayout, AV_SAMPLE_FMT_FLT, static_cast<int>(rate), 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        resampler_.reset(raw);
        if (alloc < 0 || !resampler_ || swr_init(resampler_.get()) < 0) {
            resampler_.reset();
            return false;
        }
        inRate_ = rate;
        inChannels_ = channels;
        return true;
    }

    std::string label_;
    std::mutex mutex_;
    std::unique_ptr<SwrContext, SwrDeleter> resampler_;
    std::uint32_t inRate_{0};
    std::uint32_t inChannels_{0};
    std::vector<float> samples_;
    std::optional<std::chrono::steady_clock::time_point> firstArrival_;
    std::uint64_t blocks_{0};
    double sumSquares_{0.0};
    float peak_{0.0F};
    std::optional<core::AppError> error_;
};

double dbfs(double linearRms) {
    return linearRms > 0 ? 20.0 * std::log10(linearRms) : -120.0;
}

// Generate a looping tone WAV (48k stereo 16-bit) so WASAPI loopback has real
// program audio to capture, and play it on the default render endpoint.
bool writeToneWav(const fs::path& path, int seconds) {
    const int rate = 48'000;
    const int channels = 2;
    const int frames = rate * seconds;
    const int dataBytes = frames * channels * 2;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    auto w32 = [&](std::uint32_t v) { out.write(reinterpret_cast<char*>(&v), 4); };
    auto w16 = [&](std::uint16_t v) { out.write(reinterpret_cast<char*>(&v), 2); };
    out.write("RIFF", 4);
    w32(static_cast<std::uint32_t>(36 + dataBytes));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    w32(16);
    w16(1);                                          // PCM
    w16(static_cast<std::uint16_t>(channels));
    w32(static_cast<std::uint32_t>(rate));
    w32(static_cast<std::uint32_t>(rate * channels * 2));  // byte rate
    w16(static_cast<std::uint16_t>(channels * 2));          // block align
    w16(16);                                          // bits
    out.write("data", 4);
    w32(static_cast<std::uint32_t>(dataBytes));
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        // Two-tone chord, gentle amplitude, so mean and peak are clearly audible.
        const double s = 0.35 * std::sin(2.0 * M_PI * 440.0 * t) +
                         0.25 * std::sin(2.0 * M_PI * 660.0 * t);
        const auto v = static_cast<std::int16_t>(
            std::clamp(s, -1.0, 1.0) * 32000.0);
        w16(static_cast<std::uint16_t>(v));  // L
        w16(static_cast<std::uint16_t>(v));  // R
    }
    return static_cast<bool>(out);
}

// Container mux (stream copy) of a video segment and an audio segment into one
// fragment-free .mp4. No media is re-encoded.
int muxVideoAudioToMp4(const fs::path& videoPath, const fs::path& audioPath,
                       const fs::path& output) {
    AVFormatContext* inV = nullptr;
    AVFormatContext* inA = nullptr;
    AVFormatContext* out = nullptr;
    auto cleanup = [&]() {
        if (inV) avformat_close_input(&inV);
        if (inA) avformat_close_input(&inA);
        if (out) {
            if (out->pb) avio_closep(&out->pb);
            avformat_free_context(out);
        }
    };
    int rc = avformat_open_input(&inV, videoPath.string().c_str(), nullptr, nullptr);
    if (rc < 0 || avformat_find_stream_info(inV, nullptr) < 0) {
        std::fprintf(stderr, "mux: open video failed: %s\n", avError(rc).c_str());
        cleanup();
        return 1;
    }
    rc = avformat_open_input(&inA, audioPath.string().c_str(), nullptr, nullptr);
    if (rc < 0 || avformat_find_stream_info(inA, nullptr) < 0) {
        std::fprintf(stderr, "mux: open audio failed: %s\n", avError(rc).c_str());
        cleanup();
        return 1;
    }
    const int vIn = av_find_best_stream(inV, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aIn = av_find_best_stream(inA, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vIn < 0 || aIn < 0) {
        std::fprintf(stderr, "mux: missing video(%d) or audio(%d) stream\n", vIn, aIn);
        cleanup();
        return 1;
    }
    rc = avformat_alloc_output_context2(&out, nullptr, "mp4", output.string().c_str());
    if (rc < 0 || !out) {
        std::fprintf(stderr, "mux: alloc mp4 failed: %s\n", avError(rc).c_str());
        cleanup();
        return 1;
    }
    AVStream* vOut = avformat_new_stream(out, nullptr);
    AVStream* aOut = avformat_new_stream(out, nullptr);
    if (!vOut || !aOut ||
        avcodec_parameters_copy(vOut->codecpar, inV->streams[vIn]->codecpar) < 0 ||
        avcodec_parameters_copy(aOut->codecpar, inA->streams[aIn]->codecpar) < 0) {
        std::fprintf(stderr, "mux: stream setup failed\n");
        cleanup();
        return 1;
    }
    vOut->codecpar->codec_tag = 0;
    aOut->codecpar->codec_tag = 0;
    if ((out->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = avio_open(&out->pb, output.string().c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            std::fprintf(stderr, "mux: avio_open failed: %s\n", avError(rc).c_str());
            cleanup();
            return 1;
        }
    }
    rc = avformat_write_header(out, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "mux: write header failed: %s\n", avError(rc).c_str());
        cleanup();
        return 1;
    }

    AVPacket* pkt = av_packet_alloc();
    bool moreV = true;
    bool moreA = true;
    AVPacket* pendV = av_packet_alloc();
    AVPacket* pendA = av_packet_alloc();
    bool haveV = false;
    bool haveA = false;
    auto refill = [&](AVFormatContext* in, int inIdx, AVPacket* dst, bool& have,
                      bool& more) {
        while (more && !have) {
            const int r = av_read_frame(in, pkt);
            if (r < 0) { more = false; break; }
            if (pkt->stream_index == inIdx) {
                av_packet_move_ref(dst, pkt);
                have = true;
            } else {
                av_packet_unref(pkt);
            }
        }
    };
    auto writeOne = [&](AVPacket* src, AVStream* inStream, AVStream* outStream) {
        av_packet_rescale_ts(src, inStream->time_base, outStream->time_base);
        src->stream_index = outStream->index;
        src->pos = -1;
        const int r = av_interleaved_write_frame(out, src);
        av_packet_unref(src);
        return r;
    };

    refill(inV, vIn, pendV, haveV, moreV);
    refill(inA, aIn, pendA, haveA, moreA);
    rc = 0;
    while (haveV || haveA) {
        bool writeVideo;
        if (haveV && haveA) {
            // Interleave by presentation time across the two source timebases.
            writeVideo = av_compare_ts(pendV->dts, inV->streams[vIn]->time_base,
                                       pendA->dts, inA->streams[aIn]->time_base) <= 0;
        } else {
            writeVideo = haveV;
        }
        if (writeVideo) {
            rc = writeOne(pendV, inV->streams[vIn], vOut);
            haveV = false;
            refill(inV, vIn, pendV, haveV, moreV);
        } else {
            rc = writeOne(pendA, inA->streams[aIn], aOut);
            haveA = false;
            refill(inA, aIn, pendA, haveA, moreA);
        }
        if (rc < 0) {
            std::fprintf(stderr, "mux: write frame failed: %s\n", avError(rc).c_str());
            break;
        }
    }
    av_packet_free(&pkt);
    av_packet_free(&pendV);
    av_packet_free(&pendA);
    const int trailer = av_write_trailer(out);
    if (trailer < 0 && rc == 0) rc = trailer;
    cleanup();
    return rc < 0 ? 1 : 0;
}

// Push a mixed interleaved 48k stereo buffer through the production AAC encoder.
core::Result<recorder::EncodedSegment> encodeMixedAudio(
    const std::vector<float>& mixed, const fs::path& audioSegment) {
    auto track = recorder::RecordingTrack::create(
        domain::SourceId::create("windows/mixed-audio").value(),
        recorder::TrackRole::SystemAudio);
    if (!track.hasValue()) return track.error();

    ffmpeg_adapter::FfmpegAudioSegmentEncoder encoder{ffmpeg_adapter::AudioEncoderOptions{}};
    recorder::SegmentEncodeConfig config{
        .track = std::move(track).value(),
        .partPath = audioSegment,
        .startTime = {},
        .targetDuration = {},
    };
    if (auto started = encoder.start(config); !started.hasValue()) return started.error();

    const std::size_t totalFrames = mixed.size() / kProjectChannels;
    constexpr std::uint32_t kChunkFrames = 1024;
    core::TimestampNs endTime{};
    for (std::size_t frame = 0; frame < totalFrames; frame += kChunkFrames) {
        const auto chunk =
            static_cast<std::uint32_t>(std::min<std::size_t>(kChunkFrames, totalFrames - frame));
        auto samples = std::shared_ptr<float[]>{new float[chunk * kProjectChannels]};
        std::copy_n(mixed.begin() + static_cast<std::ptrdiff_t>(frame * kProjectChannels),
                    chunk * kProjectChannels, samples.get());
        const std::int64_t ns = static_cast<std::int64_t>(
            (static_cast<double>(frame) / kProjectRate) * 1e9);
        media::AudioBlock block{
            .timestamp = core::TimestampNs{std::chrono::nanoseconds{ns}},
            .sampleRate = kProjectRate,
            .channels = kProjectChannels,
            .frameCount = chunk,
            .sampleRateRatio = 1.0,
            .samples = std::shared_ptr<const float[]>{samples},
        };
        if (auto accepted = encoder.accept(block); !accepted.hasValue()) {
            encoder.abort();
            return accepted.error();
        }
        endTime = core::TimestampNs{std::chrono::nanoseconds{static_cast<std::int64_t>(
            (static_cast<double>(frame + chunk) / kProjectRate) * 1e9)}};
    }
    return encoder.finish(endTime);
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path outputDir =
        argc > 1 ? fs::path{argv[1]} : fs::current_path() / "win-capture-demo";
    const int recordSeconds = argc > 2 ? std::atoi(argv[2]) : 5;

    std::error_code ec;
    fs::create_directories(outputDir, ec);
    const fs::path videoSegment = outputDir / "screen_av_video.mkv";
    const fs::path audioSegment = outputDir / "screen_av_audio.mka";
    const fs::path mp4Path = outputDir / "screen_with_audio.mp4";
    const fs::path toneWav = outputDir / "loopback_tone.wav";
    for (const auto& p : {videoSegment, audioSegment, mp4Path}) fs::remove(p, ec);

    std::printf("[harness] output dir : %s\n", outputDir.string().c_str());
    std::printf("[harness] record secs: %d\n", recordSeconds);
    std::fflush(stdout);

    auto backend = ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    if (!backend.screenDiscovery || !backend.screenSourceFactory || !backend.devices) {
        std::fprintf(stderr, "[harness] FATAL: capture backend is incomplete\n");
        return 2;
    }

    // ---- discover the primary display -------------------------------------
    std::promise<core::Result<std::vector<capture::ScreenCaptureTarget>>> promise;
    auto future = promise.get_future();
    backend.screenDiscovery->enumerate(
        [&promise](auto result) { promise.set_value(std::move(result)); });
    if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
        std::fprintf(stderr, "[harness] FATAL: display enumeration timed out\n");
        return 2;
    }
    auto displays = future.get();
    if (!displays.hasValue() || displays.value().empty()) {
        std::fprintf(stderr, "[harness] FATAL: no displays\n");
        return 2;
    }
    const auto& display = displays.value().front();
    std::printf("[harness] display     : %s (%ux%u)\n", display.displayName().c_str(),
                display.width(), display.height());

    // ---- video encoder + sink ---------------------------------------------
    auto vtrack = recorder::RecordingTrack::create(
        domain::SourceId::create("windows/screen").value(), recorder::TrackRole::Screen);
    if (!vtrack.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: video track: %s\n",
                     vtrack.error().message().c_str());
        return 2;
    }
    ffmpeg_adapter::FfmpegVideoSegmentEncoder videoEncoder{
        std::make_unique<ffmpeg_adapter::CpuBgraFrameMapper>(),
        ffmpeg_adapter::VideoEncoderOptions{}};
    recorder::SegmentEncodeConfig vconfig{
        .track = std::move(vtrack).value(),
        .partPath = videoSegment,
        .startTime = {},
        .targetDuration = std::chrono::seconds{recordSeconds},
    };
    auto videoSink = std::make_shared<RecordingSink>(videoEncoder, std::move(vconfig));

    // ---- audio sinks + real sources ---------------------------------------
    auto systemSink = std::make_shared<CollectingAudioSink>("system(WASAPI loopback)");
    auto micSink = std::make_shared<CollectingAudioSink>("microphone(dshow)");

    std::unique_ptr<capture::IDeviceCaptureSource> systemSource;
    std::unique_ptr<capture::IDeviceCaptureSource> micSource;

    if (auto sys = backend.devices->createSystemAudio(systemSink); sys.hasValue()) {
        systemSource = std::move(sys).value();
    } else {
        std::printf("[harness] system audio unavailable: %s\n",
                    sys.error().message().c_str());
    }

    auto mics = backend.devices->devices(capture::CaptureDeviceKind::Microphone);
    if (mics.hasValue() && !mics.value().empty()) {
        const auto& chosen = mics.value().front();
        std::printf("[harness] microphone  : %s (id=%s)\n",
                    chosen.displayName().c_str(), chosen.id().value().c_str());
        if (auto mic = backend.devices->createMicrophone(chosen.id(), micSink);
            mic.hasValue()) {
            micSource = std::move(mic).value();
        } else {
            std::printf("[harness] microphone create failed: %s\n",
                        mic.error().message().c_str());
        }
    } else {
        std::printf("[harness] no microphone device enumerated\n");
    }
    std::fflush(stdout);

    // ---- start audio, start playback, start screen ------------------------
    const capture::CaptureConfig audioCfg{};
    if (systemSource) {
        if (auto r = systemSource->start(audioCfg); !r.hasValue())
            std::printf("[harness] system start failed: %s\n", r.error().message().c_str());
    }
    if (micSource) {
        if (auto r = micSource->start(audioCfg); !r.hasValue())
            std::printf("[harness] mic start failed: %s\n", r.error().message().c_str());
    }

    bool tonePlaying = false;
    if (writeToneWav(toneWav, recordSeconds + 1)) {
        tonePlaying = PlaySoundW(toneWav.wstring().c_str(), nullptr,
                                 SND_FILENAME | SND_ASYNC | SND_LOOP) != FALSE;
        std::printf("[harness] loopback tone playback: %s\n",
                    tonePlaying ? "started (default render endpoint)" : "FAILED");
    }

    auto source = backend.screenSourceFactory->create(display.id(), videoSink);
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
        std::fprintf(stderr, "[harness] FATAL: start screen: %s\n",
                     started.error().message().c_str());
        return 2;
    }
    std::printf("[harness] capturing screen + audio for %d s...\n", recordSeconds);
    std::fflush(stdout);

    std::this_thread::sleep_for(std::chrono::seconds{recordSeconds});

    if (tonePlaying) PlaySoundW(nullptr, nullptr, 0);

    // ---- stop everything cleanly ------------------------------------------
    std::promise<core::Result<void>> stopPromise;
    auto stopFuture = stopPromise.get_future();
    source.value()->stopAsync(
        [&stopPromise](auto result) { stopPromise.set_value(std::move(result)); });
    stopFuture.wait_for(std::chrono::seconds{10});

    auto stopDevice = [](std::unique_ptr<capture::IDeviceCaptureSource>& s) {
        if (!s) return;
        std::promise<core::Result<void>> p;
        auto f = p.get_future();
        s->stopAsync([&p](auto r) { p.set_value(std::move(r)); });
        f.wait_for(std::chrono::seconds{10});
    };
    stopDevice(systemSource);
    stopDevice(micSource);

    const auto vsnap = videoSink->snapshot();
    std::printf("[harness] video frames accepted=%llu skipped=%llu\n",
                static_cast<unsigned long long>(vsnap.accepted),
                static_cast<unsigned long long>(vsnap.skipped));
    if (vsnap.error) {
        std::fprintf(stderr, "[harness] FATAL: video capture error: %s\n",
                     vsnap.error->message().c_str());
        return 3;
    }
    if (!vsnap.started || vsnap.accepted == 0) {
        std::fprintf(stderr, "[harness] FATAL: no video frames encoded\n");
        return 3;
    }
    if (auto finished = videoEncoder.finish(vsnap.lastTimestamp); !finished.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: finish video: %s\n",
                     finished.error().message().c_str());
        return 3;
    }

    // ---- collect + mix audio ----------------------------------------------
    auto sysRes = systemSink->take();
    auto micRes = micSink->take();
    std::printf("[harness] %-24s blocks=%llu frames=%zu rms=%.1f dBFS peak=%.1f dBFS%s\n",
                sysRes.label.c_str(), (unsigned long long)sysRes.blocks,
                sysRes.samples.size() / kProjectChannels, sysRes.rmsDbfs, sysRes.peakDbfs,
                sysRes.error ? "  [ERR]" : "");
    if (sysRes.error)
        std::printf("[harness]   system error: %s\n", sysRes.error->message().c_str());
    std::printf("[harness] %-24s blocks=%llu frames=%zu rms=%.1f dBFS peak=%.1f dBFS%s\n",
                micRes.label.c_str(), (unsigned long long)micRes.blocks,
                micRes.samples.size() / kProjectChannels, micRes.rmsDbfs, micRes.peakDbfs,
                micRes.error ? "  [ERR]" : "");
    if (micRes.error)
        std::printf("[harness]   mic error: %s\n", micRes.error->message().c_str());
    std::fflush(stdout);

    // Align the two sources on a shared steady-clock origin (their first block
    // arrival) so summed samples stay time-coherent.
    std::optional<std::chrono::steady_clock::time_point> origin;
    for (auto* r : {&sysRes, &micRes}) {
        if (r->firstArrival && (!origin || *r->firstArrival < *origin))
            origin = r->firstArrival;
    }
    std::vector<float> mixed;
    if (origin) {
        std::size_t totalFrames = 0;
        auto offsetFrames = [&](const CollectingAudioSink::Result& r) -> std::size_t {
            if (!r.firstArrival) return 0;
            const auto delta = std::chrono::duration_cast<std::chrono::duration<double>>(
                                   *r.firstArrival - *origin)
                                   .count();
            return static_cast<std::size_t>(std::llround(delta * kProjectRate));
        };
        for (auto* r : {&sysRes, &micRes}) {
            totalFrames = std::max(totalFrames,
                                   offsetFrames(*r) + r->samples.size() / kProjectChannels);
        }
        mixed.assign(totalFrames * kProjectChannels, 0.0F);
        for (auto* r : {&sysRes, &micRes}) {
            const std::size_t off = offsetFrames(*r) * kProjectChannels;
            for (std::size_t i = 0; i < r->samples.size(); ++i)
                mixed[off + i] += r->samples[i];
        }
        for (float& v : mixed) v = std::clamp(v, -1.0F, 1.0F);
    }

    // Measure the mixed signal.
    double mixedRms = -120.0;
    double mixedPeak = -120.0;
    if (!mixed.empty()) {
        double sumSq = 0.0;
        float peak = 0.0F;
        for (float v : mixed) {
            sumSq += static_cast<double>(v) * v;
            peak = std::max(peak, std::fabs(v));
        }
        mixedRms = dbfs(std::sqrt(sumSq / mixed.size()));
        mixedPeak = peak > 0 ? 20.0 * std::log10(peak) : -120.0;
    }

    bool synthesizedFallback = false;
    if (mixed.empty() || mixedPeak < -80.0) {
        // Both real sources were silent/absent: prove the encode+mux path with a
        // clearly-labeled synthesized tone rather than shipping silence.
        synthesizedFallback = true;
        const std::size_t frames = static_cast<std::size_t>(kProjectRate) * recordSeconds;
        mixed.assign(frames * kProjectChannels, 0.0F);
        for (std::size_t f = 0; f < frames; ++f) {
            const double t = static_cast<double>(f) / kProjectRate;
            const float v = static_cast<float>(0.4 * std::sin(2.0 * M_PI * 440.0 * t));
            mixed[f * kProjectChannels] = v;
            mixed[f * kProjectChannels + 1] = v;
        }
        std::printf("[harness] *** WARNING: real capture was silent; using SYNTHESIZED "
                    "440Hz tone to prove the mux/encode path ***\n");
    }
    std::printf("[harness] MIXED audio         frames=%zu rms=%.1f dBFS peak=%.1f dBFS%s\n",
                mixed.size() / kProjectChannels, mixedRms, mixedPeak,
                synthesizedFallback ? "  [SYNTHESIZED]" : "  [REAL CAPTURE]");
    std::fflush(stdout);

    auto audioEncoded = encodeMixedAudio(mixed, audioSegment);
    if (!audioEncoded.hasValue()) {
        std::fprintf(stderr, "[harness] FATAL: encode audio: %s\n",
                     audioEncoded.error().message().c_str());
        return 4;
    }
    std::printf("[harness] audio segment codec=%s bytes=%llu -> %s\n",
                audioEncoded.value().codecName.c_str(),
                (unsigned long long)audioEncoded.value().bytesWritten,
                audioSegment.string().c_str());

    if (const int rc = muxVideoAudioToMp4(videoSegment, audioSegment, mp4Path); rc != 0) {
        std::fprintf(stderr, "[harness] FATAL: mux failed\n");
        return 5;
    }
    const auto bytes = fs::file_size(mp4Path, ec);
    std::printf("[harness] OK mp4=%s bytes=%llu\n", mp4Path.string().c_str(),
                static_cast<unsigned long long>(bytes));
    std::printf("[harness] audio_source=%s\n",
                synthesizedFallback ? "SYNTHESIZED_TONE_FALLBACK"
                                    : "REAL(system-loopback + microphone, mixed)");
    return 0;
}
