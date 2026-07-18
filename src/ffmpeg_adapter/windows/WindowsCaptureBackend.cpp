#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"

#include "capture/AudioCaptureBlockAssembler.h"
#include "capture/CameraCaptureFrameAssembler.h"
#include "capture/NumericCaptureTargetId.h"
#include "capture/ScreenCaptureFrameAssembler.h"
#include "core/AppError.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define NOMINMAX
#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace creator::ffmpeg_adapter::windows {
namespace {

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidArgument, std::move(message)};
}

core::AppError ioFailure(std::string message) {
    return {core::ErrorCode::IoFailure, std::move(message)};
}

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0) return "unknown FFmpeg error";
    return buffer;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

struct MonitorSpec final {
    std::string id;
    std::string name;
    int left{};
    int top{};
    int width{};
    int height{};
};

class CaptureRegistry final {
public:
    void replaceMonitors(std::vector<MonitorSpec> monitors) {
        std::lock_guard lock(mutex_);
        monitors_.clear();
        for (auto& monitor : monitors) {
            monitors_.emplace(monitor.id, std::move(monitor));
        }
    }

    std::optional<MonitorSpec> monitor(std::string_view id) const {
        std::lock_guard lock(mutex_);
        const auto found = monitors_.find(std::string{id});
        return found == monitors_.end()
                   ? std::optional<MonitorSpec>{}
                   : std::optional<MonitorSpec>{found->second};
    }

    void replaceDevices(capture::CaptureDeviceKind kind,
                        std::unordered_map<std::string, std::string> devices) {
        std::lock_guard lock(mutex_);
        (kind == capture::CaptureDeviceKind::Camera ? cameras_ : microphones_) =
            std::move(devices);
    }

    std::optional<std::string> device(capture::CaptureDeviceKind kind,
                                      std::string_view id) const {
        std::lock_guard lock(mutex_);
        const auto& devices = kind == capture::CaptureDeviceKind::Camera
                                  ? cameras_
                                  : microphones_;
        const auto found = devices.find(std::string{id});
        return found == devices.end() ? std::optional<std::string>{}
                                      : std::optional<std::string>{found->second};
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, MonitorSpec> monitors_;
    std::unordered_map<std::string, std::string> cameras_;
    std::unordered_map<std::string, std::string> microphones_;
};

class ScreenPermission final : public capture::IScreenCapturePermission {
public:
    capture::ScreenCapturePermissionStatus status() const noexcept override {
        return capture::ScreenCapturePermissionStatus::Granted;
    }
    void request(Completion completion) override {
        if (completion) completion(capture::ScreenCapturePermissionStatus::Granted);
    }
};

struct MonitorEnumeration final {
    std::vector<MonitorSpec> specs;
    std::vector<capture::ScreenCaptureTarget> targets;
    std::optional<core::AppError> error;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& output = *reinterpret_cast<MonitorEnumeration*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        output.error = ioFailure("Windows could not inspect a display");
        return FALSE;
    }
    const auto nativeId = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(monitor));
    auto id = capture::makeNumericCaptureTargetId(
        capture::ScreenCaptureTargetKind::Display, nativeId);
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    const std::string name = utf8(info.szDevice);
    if (!id.hasValue() || width <= 0 || height <= 0 || name.empty()) {
        output.error = invalid("Windows returned invalid display metadata");
        return FALSE;
    }
    auto target = capture::ScreenCaptureTarget::create(
        id.value(), capture::ScreenCaptureTargetKind::Display, name,
        std::nullopt, static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height));
    if (!target.hasValue()) {
        output.error = target.error();
        return FALSE;
    }
    output.specs.push_back(MonitorSpec{.id = id.value().value(),
                                       .name = name,
                                       .left = info.rcMonitor.left,
                                       .top = info.rcMonitor.top,
                                       .width = width,
                                       .height = height});
    output.targets.push_back(std::move(target).value());
    return TRUE;
}

class ScreenDiscovery final : public capture::IScreenCaptureDiscovery {
public:
    explicit ScreenDiscovery(std::shared_ptr<CaptureRegistry> registry)
        : registry_(std::move(registry)) {}

    void enumerate(Completion completion) override {
        MonitorEnumeration output;
        if (!EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                                 reinterpret_cast<LPARAM>(&output)) ||
            output.error.has_value()) {
            if (completion) {
                completion(output.error.value_or(
                    ioFailure("Windows could not enumerate displays")));
            }
            return;
        }
        registry_->replaceMonitors(std::move(output.specs));
        if (completion) completion(std::move(output.targets));
    }

private:
    std::shared_ptr<CaptureRegistry> registry_;
};

struct FormatDeleter final {
    void operator()(AVFormatContext* value) const noexcept {
        if (value) avformat_close_input(&value);
    }
};
struct CodecDeleter final {
    void operator()(AVCodecContext* value) const noexcept {
        if (value) avcodec_free_context(&value);
    }
};
struct FrameDeleter final {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};
struct PacketDeleter final {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};
struct ScaleDeleter final {
    void operator()(SwsContext* value) const noexcept { sws_freeContext(value); }
};
struct ResampleDeleter final {
    void operator()(SwrContext* value) const noexcept { swr_free(&value); }
};
struct DeviceListDeleter final {
    void operator()(AVDeviceInfoList* value) const noexcept {
        if (value) avdevice_free_list_devices(&value);
    }
};

class ScreenSource final : public capture::IScreenCaptureSource {
public:
    ScreenSource(MonitorSpec monitor,
                 std::shared_ptr<capture::IVideoFrameSink> sink)
        : monitor_(std::move(monitor)), sink_(std::move(sink)),
          id_(domain::SourceId::create("windows/" + monitor_.id).value()) {}

    ~ScreenSource() override { static_cast<void>(stop()); }

    domain::SourceId id() const override { return id_; }
    std::string displayName() const override { return monitor_.name; }

    core::Result<void> start(const capture::CaptureConfig& config) override {
        if (config.frameRateNumerator == 0 ||
            config.frameRateDenominator == 0) {
            return invalid("screen frame rate is invalid");
        }
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return core::AppError{
                core::ErrorCode::InvalidState,
                "Windows screen capture is already started"};
        }
        stopRequested_.store(false);
        worker_ = std::jthread{[this, config](std::stop_token) { run(config); }};
        return core::ok();
    }

    core::Result<void> stop() override {
        stopRequested_.store(true, std::memory_order_release);
        if (stopThread_.joinable()) stopThread_.join();
        else if (worker_.joinable()) worker_.join();
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (stopThread_.joinable()) {
            if (completion) completion(core::AppError{
                core::ErrorCode::InvalidState,
                "Windows screen stop is already in progress"});
            return;
        }
        stopRequested_.store(true, std::memory_order_release);
        stopThread_ = std::thread{[this, completion = std::move(completion)]() mutable {
            if (worker_.joinable()) worker_.join();
            if (completion) completion(core::ok());
        }};
    }

    capture::CaptureStats stats() const noexcept override {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    static int interrupt(void* opaque) noexcept {
        return static_cast<ScreenSource*>(opaque)->stopRequested_.load(
                   std::memory_order_acquire)
                   ? 1
                   : 0;
    }

    void fail(core::AppError error) noexcept {
        std::shared_ptr<capture::IVideoFrameSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (terminalSent_ || stopRequested_.load()) return;
            terminalSent_ = true;
            sink = sink_;
        }
        if (sink) sink->onCaptureError(std::move(error));
    }

    void run(const capture::CaptureConfig& config) noexcept {
        try {
            avdevice_register_all();
            const auto* input = av_find_input_format("gdigrab");
            if (!input) {
                fail(ioFailure("audited FFmpeg gdigrab input is unavailable"));
                return;
            }
            AVFormatContext* rawFormat = avformat_alloc_context();
            if (!rawFormat) {
                fail(ioFailure("screen demuxer allocation failed"));
                return;
            }
            rawFormat->interrupt_callback = {interrupt, this};
            AVDictionary* options = nullptr;
            const auto rate = std::to_string(config.frameRateNumerator) + "/" +
                              std::to_string(config.frameRateDenominator);
            const auto size = std::to_string(monitor_.width) + "x" +
                              std::to_string(monitor_.height);
            av_dict_set(&options, "framerate", rate.c_str(), 0);
            av_dict_set(&options, "video_size", size.c_str(), 0);
            av_dict_set(&options, "offset_x", std::to_string(monitor_.left).c_str(), 0);
            av_dict_set(&options, "offset_y", std::to_string(monitor_.top).c_str(), 0);
            av_dict_set(&options, "draw_mouse", "1", 0);
            const int opened = avformat_open_input(
                &rawFormat, "desktop", input, &options);
            av_dict_free(&options);
            if (opened < 0) {
                if (rawFormat) avformat_free_context(rawFormat);
                fail(ioFailure("screen input open failed: " +
                               ffmpegError(opened)));
                return;
            }
            std::unique_ptr<AVFormatContext, FormatDeleter> format{rawFormat};
            if (avformat_find_stream_info(format.get(), nullptr) < 0) {
                fail(ioFailure("screen stream metadata is unavailable"));
                return;
            }
            const int streamIndex = av_find_best_stream(
                format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (streamIndex < 0) {
                fail(ioFailure("screen input has no video stream"));
                return;
            }
            auto* stream = format->streams[streamIndex];
            const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!decoder) {
                fail(ioFailure("screen decoder is unavailable"));
                return;
            }
            std::unique_ptr<AVCodecContext, CodecDeleter> codec{
                avcodec_alloc_context3(decoder)};
            if (!codec ||
                avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
                avcodec_open2(codec.get(), decoder, nullptr) < 0) {
                fail(ioFailure("screen decoder could not start"));
                return;
            }
            std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
            std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
            if (!packet || !frame) {
                fail(ioFailure("screen frame allocation failed"));
                return;
            }
            if (sink_) sink_->onCaptureStarted();
            while (!stopRequested_.load(std::memory_order_acquire)) {
                const int read = av_read_frame(format.get(), packet.get());
                if (read == AVERROR_EXIT && stopRequested_.load()) break;
                if (read < 0) {
                    fail(ioFailure("screen frame read failed: " + ffmpegError(read)));
                    break;
                }
                if (packet->stream_index == streamIndex) {
                    const int sent = avcodec_send_packet(codec.get(), packet.get());
                    if (sent >= 0) {
                        while (avcodec_receive_frame(codec.get(), frame.get()) >= 0) {
                            deliver(*frame, stream->time_base);
                            av_frame_unref(frame.get());
                        }
                    }
                }
                av_packet_unref(packet.get());
            }
        } catch (...) {
            fail({core::ErrorCode::Unknown,
                  "Windows screen capture worker failed"});
        }
    }

    void deliver(const AVFrame& frame, AVRational timeBase) {
        if (frame.width <= 0 || frame.height <= 0) return;
        std::unique_ptr<SwsContext, ScaleDeleter> scale{sws_getContext(
            frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
            frame.width, frame.height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
            nullptr, nullptr)};
        if (!scale) {
            fail(ioFailure("screen BGRA conversion is unavailable"));
            return;
        }
        const auto stride = static_cast<std::size_t>(frame.width) * 4U;
        const auto bytes = stride * static_cast<std::size_t>(frame.height);
        auto pixels = std::shared_ptr<std::uint8_t[]>{new std::uint8_t[bytes]};
        std::uint8_t* destinations[]{pixels.get()};
        int strides[]{static_cast<int>(stride)};
        if (sws_scale(scale.get(), frame.data, frame.linesize, 0, frame.height,
                      destinations, strides) != frame.height) {
            fail(ioFailure("screen BGRA conversion failed"));
            return;
        }
        std::int64_t pts = frame.best_effort_timestamp;
        capture::NativeTimestamp timestamp;
        if (pts != AV_NOPTS_VALUE && timeBase.num > 0 && timeBase.den > 0 &&
            pts <= std::numeric_limits<std::int64_t>::max() / timeBase.num &&
            pts >= std::numeric_limits<std::int64_t>::min() / timeBase.num) {
            timestamp = {pts * timeBase.num, timeBase.den};
        } else {
            timestamp = {std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count(),
                         1'000'000'000};
        }
        auto handle = std::shared_ptr<void>{pixels, pixels.get()};
        auto assembled = assembler_.assemble(capture::NativeScreenFrame{
            .status = capture::NativeScreenFrameStatus::Complete,
            .timestamp = timestamp,
            .width = static_cast<std::uint32_t>(frame.width),
            .height = static_cast<std::uint32_t>(frame.height),
            .visibleRect = {.width = static_cast<std::uint32_t>(frame.width),
                            .height = static_cast<std::uint32_t>(frame.height)},
            .contentWidth = static_cast<std::uint32_t>(frame.width),
            .contentHeight = static_cast<std::uint32_t>(frame.height),
            .pixelFormat = media::PixelFormat::Bgra8,
            .platformHandle = std::move(handle)});
        if (!assembled.hasValue() || !assembled.value().has_value()) {
            std::lock_guard lock(mutex_);
            ++stats_.invalidFrames;
            return;
        }
        std::shared_ptr<capture::IVideoFrameSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_.load()) return;
            if (lastTimestamp_.has_value()) {
                const auto delta = assembled.value()->timestamp - *lastTimestamp_;
                if (delta.count() > 0) {
                    stats_.currentFps = 1'000'000'000.0 /
                                        static_cast<double>(delta.count());
                }
            }
            lastTimestamp_ = assembled.value()->timestamp;
            ++stats_.receivedFrames;
            sink = sink_;
        }
        if (sink) sink->onVideoFrame(std::move(*assembled.value()));
    }

    MonitorSpec monitor_;
    std::shared_ptr<capture::IVideoFrameSink> sink_;
    domain::SourceId id_;
    capture::ScreenCaptureFrameAssembler assembler_;
    std::atomic_bool started_{};
    std::atomic_bool stopRequested_{};
    std::jthread worker_;
    std::thread stopThread_;
    mutable std::mutex mutex_;
    capture::CaptureStats stats_;
    std::optional<core::TimestampNs> lastTimestamp_;
    bool terminalSent_{};
};

class ScreenFactory final : public capture::IScreenCaptureSourceFactory {
public:
    explicit ScreenFactory(std::shared_ptr<CaptureRegistry> registry)
        : registry_(std::move(registry)) {}

    core::Result<std::unique_ptr<capture::IScreenCaptureSource>> create(
        const domain::CaptureTargetId& targetId,
        std::shared_ptr<capture::IVideoFrameSink> sink) override {
        if (!sink) return invalid("screen sink is required");
        auto monitor = registry_->monitor(targetId.value());
        if (!monitor.has_value()) {
            return core::AppError{
                core::ErrorCode::NotFound,
                "selected Windows display is no longer available"};
        }
        std::unique_ptr<capture::IScreenCaptureSource> source =
            std::make_unique<ScreenSource>(std::move(*monitor), std::move(sink));
        return source;
    }

private:
    std::shared_ptr<CaptureRegistry> registry_;
};

std::string dshowUrl(std::string_view mediaPrefix, std::string_view deviceName) {
    const std::string prefix{mediaPrefix};
    if (deviceName.starts_with(prefix)) return std::string{deviceName};
    return prefix + std::string{deviceName};
}

class CameraSource final : public capture::IDeviceCaptureSource {
public:
    CameraSource(domain::CaptureDeviceId deviceId, std::string deviceName,
                 std::shared_ptr<capture::IVideoFrameSink> sink)
        : deviceId_(std::move(deviceId)), deviceName_(std::move(deviceName)),
          sink_(std::move(sink)),
          id_(domain::SourceId::create("windows/" + deviceId_.value()).value()) {}

    ~CameraSource() override { static_cast<void>(stop()); }

    domain::SourceId id() const override { return id_; }
    std::string displayName() const override { return deviceName_; }

    core::Result<void> start(const capture::CaptureConfig& config) override {
        if (config.frameRateNumerator == 0 ||
            config.frameRateDenominator == 0) {
            return invalid("camera frame rate is invalid");
        }
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Windows camera is already started"};
        }
        stopRequested_.store(false, std::memory_order_release);
        worker_ = std::jthread{[this, config](std::stop_token) { run(config); }};
        return core::ok();
    }

    core::Result<void> stop() override {
        stopRequested_.store(true, std::memory_order_release);
        if (stopThread_.joinable()) stopThread_.join();
        else if (worker_.joinable()) worker_.join();
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (stopThread_.joinable()) {
            if (completion) completion(core::AppError{
                core::ErrorCode::InvalidState,
                "Windows camera stop is already in progress"});
            return;
        }
        stopRequested_.store(true, std::memory_order_release);
        stopThread_ = std::thread{[this, completion = std::move(completion)]() mutable {
            if (worker_.joinable()) worker_.join();
            if (completion) completion(core::ok());
        }};
    }

    capture::CaptureStats stats() const noexcept override {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    static int interrupt(void* opaque) noexcept {
        return static_cast<CameraSource*>(opaque)->stopRequested_.load(
                   std::memory_order_acquire)
                   ? 1
                   : 0;
    }

    void fail(core::AppError error) noexcept {
        std::shared_ptr<capture::IVideoFrameSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (terminalSent_ || stopRequested_.load()) return;
            terminalSent_ = true;
            sink = sink_;
        }
        if (sink) sink->onCaptureError(std::move(error));
    }

    core::Result<std::unique_ptr<AVFormatContext, FormatDeleter>> open(
        const capture::CaptureConfig& config) {
        const auto* input = av_find_input_format("dshow");
        if (!input) return ioFailure("audited FFmpeg dshow input is unavailable");
        AVFormatContext* raw = avformat_alloc_context();
        if (!raw) return ioFailure("camera demuxer allocation failed");
        raw->interrupt_callback = {interrupt, this};
        AVDictionary* options = nullptr;
        const auto rate = std::to_string(config.frameRateNumerator) + "/" +
                          std::to_string(config.frameRateDenominator);
        const auto size = std::to_string(config.targetWidth) + "x" +
                          std::to_string(config.targetHeight);
        av_dict_set(&options, "framerate", rate.c_str(), 0);
        av_dict_set(&options, "video_size", size.c_str(), 0);
        const auto url = dshowUrl("video=", deviceName_);
        const int opened = avformat_open_input(&raw, url.c_str(), input, &options);
        av_dict_free(&options);
        if (opened < 0) {
            if (raw) avformat_free_context(raw);
            return ioFailure("camera input open failed: " + ffmpegError(opened));
        }
        return std::unique_ptr<AVFormatContext, FormatDeleter>{raw};
    }

    void run(const capture::CaptureConfig& config) noexcept {
        try {
            avdevice_register_all();
            auto opened = open(config);
            if (!opened.hasValue()) {
                fail(opened.error());
                return;
            }
            auto format = std::move(opened).value();
            if (avformat_find_stream_info(format.get(), nullptr) < 0) {
                fail(ioFailure("camera stream metadata is unavailable"));
                return;
            }
            const int streamIndex = av_find_best_stream(
                format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (streamIndex < 0) {
                fail(ioFailure("camera input has no video stream"));
                return;
            }
            auto* stream = format->streams[streamIndex];
            const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!decoder) {
                fail(ioFailure("camera decoder is unavailable"));
                return;
            }
            std::unique_ptr<AVCodecContext, CodecDeleter> codec{
                avcodec_alloc_context3(decoder)};
            if (!codec ||
                avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
                avcodec_open2(codec.get(), decoder, nullptr) < 0) {
                fail(ioFailure("camera decoder could not start"));
                return;
            }
            std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
            std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
            if (!packet || !frame) {
                fail(ioFailure("camera frame allocation failed"));
                return;
            }
            if (sink_) sink_->onCaptureStarted();
            while (!stopRequested_.load(std::memory_order_acquire)) {
                const int read = av_read_frame(format.get(), packet.get());
                if (read == AVERROR_EXIT && stopRequested_.load()) break;
                if (read < 0) {
                    fail(ioFailure("camera frame read failed: " + ffmpegError(read)));
                    break;
                }
                if (packet->stream_index == streamIndex &&
                    avcodec_send_packet(codec.get(), packet.get()) >= 0) {
                    while (avcodec_receive_frame(codec.get(), frame.get()) >= 0) {
                        deliver(*frame, stream->time_base);
                        av_frame_unref(frame.get());
                    }
                }
                av_packet_unref(packet.get());
            }
        } catch (...) {
            fail({core::ErrorCode::Unknown,
                  "Windows camera capture worker failed"});
        }
    }

    void deliver(const AVFrame& frame, AVRational timeBase) {
        if (frame.width <= 0 || frame.height <= 0) return;
        std::unique_ptr<SwsContext, ScaleDeleter> scale{sws_getContext(
            frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
            frame.width, frame.height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
            nullptr, nullptr)};
        if (!scale) {
            fail(ioFailure("camera BGRA conversion is unavailable"));
            return;
        }
        const auto stride = static_cast<std::size_t>(frame.width) * 4U;
        const auto bytes = stride * static_cast<std::size_t>(frame.height);
        auto pixels = std::shared_ptr<std::uint8_t[]>{new std::uint8_t[bytes]};
        std::uint8_t* destinations[]{pixels.get()};
        int strides[]{static_cast<int>(stride)};
        if (sws_scale(scale.get(), frame.data, frame.linesize, 0, frame.height,
                      destinations, strides) != frame.height) {
            fail(ioFailure("camera BGRA conversion failed"));
            return;
        }
        const auto timestamp = nativeTimestamp(frame.best_effort_timestamp, timeBase);
        auto handle = std::shared_ptr<void>{pixels, pixels.get()};
        auto assembled = assembler_.assemble(capture::NativeCameraFrame{
            .timestamp = timestamp,
            .width = static_cast<std::uint32_t>(frame.width),
            .height = static_cast<std::uint32_t>(frame.height),
            .pixelFormat = media::PixelFormat::Bgra8,
            .platformHandle = std::move(handle)});
        if (!assembled.hasValue()) {
            std::lock_guard lock(mutex_);
            ++stats_.invalidFrames;
            return;
        }
        std::shared_ptr<capture::IVideoFrameSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_.load()) return;
            if (lastTimestamp_.has_value()) {
                const auto delta = assembled.value().timestamp - *lastTimestamp_;
                if (delta.count() > 0) {
                    stats_.currentFps = 1'000'000'000.0 /
                                        static_cast<double>(delta.count());
                }
            }
            lastTimestamp_ = assembled.value().timestamp;
            ++stats_.receivedFrames;
            sink = sink_;
        }
        if (sink) sink->onVideoFrame(std::move(assembled).value());
    }

    static capture::NativeTimestamp nativeTimestamp(std::int64_t pts,
                                                     AVRational timeBase) {
        if (pts != AV_NOPTS_VALUE && timeBase.num > 0 && timeBase.den > 0 &&
            pts <= std::numeric_limits<std::int64_t>::max() / timeBase.num &&
            pts >= std::numeric_limits<std::int64_t>::min() / timeBase.num) {
            return {pts * timeBase.num, timeBase.den};
        }
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                1'000'000'000};
    }

    domain::CaptureDeviceId deviceId_;
    std::string deviceName_;
    std::shared_ptr<capture::IVideoFrameSink> sink_;
    domain::SourceId id_;
    capture::CameraCaptureFrameAssembler assembler_;
    std::atomic_bool started_{};
    std::atomic_bool stopRequested_{};
    std::jthread worker_;
    std::thread stopThread_;
    mutable std::mutex mutex_;
    capture::CaptureStats stats_;
    std::optional<core::TimestampNs> lastTimestamp_;
    bool terminalSent_{};
};

class MicrophoneSource final : public capture::IDeviceCaptureSource {
public:
    MicrophoneSource(domain::CaptureDeviceId deviceId, std::string deviceName,
                     std::shared_ptr<capture::IAudioBlockSink> sink)
        : deviceId_(std::move(deviceId)), deviceName_(std::move(deviceName)),
          sink_(std::move(sink)),
          id_(domain::SourceId::create("windows/" + deviceId_.value()).value()) {}

    ~MicrophoneSource() override { static_cast<void>(stop()); }

    domain::SourceId id() const override { return id_; }
    std::string displayName() const override { return deviceName_; }

    core::Result<void> start(const capture::CaptureConfig&) override {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Windows microphone is already started"};
        }
        stopRequested_.store(false, std::memory_order_release);
        worker_ = std::jthread{[this](std::stop_token) { run(); }};
        return core::ok();
    }

    core::Result<void> stop() override {
        stopRequested_.store(true, std::memory_order_release);
        if (stopThread_.joinable()) stopThread_.join();
        else if (worker_.joinable()) worker_.join();
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (stopThread_.joinable()) {
            if (completion) completion(core::AppError{
                core::ErrorCode::InvalidState,
                "Windows microphone stop is already in progress"});
            return;
        }
        stopRequested_.store(true, std::memory_order_release);
        stopThread_ = std::thread{[this, completion = std::move(completion)]() mutable {
            if (worker_.joinable()) worker_.join();
            if (completion) completion(core::ok());
        }};
    }

    capture::CaptureStats stats() const noexcept override {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    static int interrupt(void* opaque) noexcept {
        return static_cast<MicrophoneSource*>(opaque)->stopRequested_.load(
                   std::memory_order_acquire)
                   ? 1
                   : 0;
    }

    void fail(core::AppError error) noexcept {
        std::shared_ptr<capture::IAudioBlockSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (terminalSent_ || stopRequested_.load()) return;
            terminalSent_ = true;
            sink = sink_;
        }
        if (sink) sink->onCaptureError(std::move(error));
    }

    void run() noexcept {
        try {
            avdevice_register_all();
            const auto* input = av_find_input_format("dshow");
            if (!input) {
                fail(ioFailure("audited FFmpeg dshow input is unavailable"));
                return;
            }
            AVFormatContext* rawFormat = avformat_alloc_context();
            if (!rawFormat) {
                fail(ioFailure("microphone demuxer allocation failed"));
                return;
            }
            rawFormat->interrupt_callback = {interrupt, this};
            const auto url = dshowUrl("audio=", deviceName_);
            const int opened = avformat_open_input(
                &rawFormat, url.c_str(), input, nullptr);
            if (opened < 0) {
                if (rawFormat) avformat_free_context(rawFormat);
                fail(ioFailure("microphone input open failed: " +
                               ffmpegError(opened)));
                return;
            }
            std::unique_ptr<AVFormatContext, FormatDeleter> format{rawFormat};
            if (avformat_find_stream_info(format.get(), nullptr) < 0) {
                fail(ioFailure("microphone stream metadata is unavailable"));
                return;
            }
            const int streamIndex = av_find_best_stream(
                format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (streamIndex < 0) {
                fail(ioFailure("microphone input has no audio stream"));
                return;
            }
            auto* stream = format->streams[streamIndex];
            const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!decoder) {
                fail(ioFailure("microphone decoder is unavailable"));
                return;
            }
            std::unique_ptr<AVCodecContext, CodecDeleter> codec{
                avcodec_alloc_context3(decoder)};
            if (!codec ||
                avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
                avcodec_open2(codec.get(), decoder, nullptr) < 0) {
                fail(ioFailure("microphone decoder could not start"));
                return;
            }
            std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
            std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
            if (!packet || !frame) {
                fail(ioFailure("microphone frame allocation failed"));
                return;
            }
            if (sink_) sink_->onCaptureStarted();
            while (!stopRequested_.load(std::memory_order_acquire)) {
                const int read = av_read_frame(format.get(), packet.get());
                if (read == AVERROR_EXIT && stopRequested_.load()) break;
                if (read < 0) {
                    fail(ioFailure("microphone block read failed: " +
                                   ffmpegError(read)));
                    break;
                }
                if (packet->stream_index == streamIndex &&
                    avcodec_send_packet(codec.get(), packet.get()) >= 0) {
                    while (avcodec_receive_frame(codec.get(), frame.get()) >= 0) {
                        deliver(*frame, stream->time_base);
                        av_frame_unref(frame.get());
                    }
                }
                av_packet_unref(packet.get());
            }
        } catch (...) {
            fail({core::ErrorCode::Unknown,
                  "Windows microphone capture worker failed"});
        }
    }

    void deliver(const AVFrame& frame, AVRational timeBase) {
        if (frame.nb_samples <= 0 || frame.sample_rate <= 0) return;
        AVChannelLayout inputLayout{};
        if (frame.ch_layout.nb_channels > 0) {
            if (av_channel_layout_copy(&inputLayout, &frame.ch_layout) < 0) {
                fail(ioFailure("microphone channel layout copy failed"));
                return;
            }
        } else {
            av_channel_layout_default(&inputLayout, 2);
        }
        AVChannelLayout outputLayout{};
        av_channel_layout_default(&outputLayout, inputLayout.nb_channels);
        SwrContext* rawResampler = nullptr;
        const int allocated = swr_alloc_set_opts2(
            &rawResampler, &outputLayout, AV_SAMPLE_FMT_FLT, frame.sample_rate,
            &inputLayout, static_cast<AVSampleFormat>(frame.format),
            frame.sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        av_channel_layout_uninit(&outputLayout);
        std::unique_ptr<SwrContext, ResampleDeleter> resampler{rawResampler};
        if (allocated < 0 || !resampler || swr_init(resampler.get()) < 0) {
            fail(ioFailure("microphone float conversion is unavailable"));
            return;
        }
        const auto channels = static_cast<std::uint32_t>(frame.ch_layout.nb_channels > 0
                                                             ? frame.ch_layout.nb_channels
                                                             : 2);
        const int maximumFrames = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler.get(), frame.sample_rate) + frame.nb_samples,
            frame.sample_rate, frame.sample_rate, AV_ROUND_UP));
        if (maximumFrames <= 0) return;
        const auto maximumSamples = static_cast<std::size_t>(maximumFrames) *
                                    static_cast<std::size_t>(channels);
        auto samples = std::shared_ptr<float[]>{new float[maximumSamples]};
        std::uint8_t* output[]{reinterpret_cast<std::uint8_t*>(samples.get())};
        const auto* inputData =
            const_cast<const std::uint8_t**>(frame.extended_data);
        const int converted = swr_convert(resampler.get(), output, maximumFrames,
                                          inputData, frame.nb_samples);
        if (converted <= 0) {
            fail(ioFailure("microphone float conversion failed"));
            return;
        }
        const auto sampleCount = static_cast<std::size_t>(converted) * channels;
        auto assembled = assembler_.assemble(capture::NativeAudioBlock{
            .timestamp = nativeTimestamp(frame.best_effort_timestamp, timeBase),
            .sampleRate = static_cast<std::uint32_t>(frame.sample_rate),
            .channels = channels,
            .frameCount = static_cast<std::uint32_t>(converted),
            .sampleCount = sampleCount,
            .samples = std::move(samples)});
        if (!assembled.hasValue()) {
            std::lock_guard lock(mutex_);
            ++stats_.invalidFrames;
            return;
        }
        std::shared_ptr<capture::IAudioBlockSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_.load()) return;
            ++stats_.receivedFrames;
            sink = sink_;
        }
        if (sink) sink->onAudioBlock(std::move(assembled).value());
    }

    static capture::NativeTimestamp nativeTimestamp(std::int64_t pts,
                                                     AVRational timeBase) {
        if (pts != AV_NOPTS_VALUE && timeBase.num > 0 && timeBase.den > 0 &&
            pts <= std::numeric_limits<std::int64_t>::max() / timeBase.num &&
            pts >= std::numeric_limits<std::int64_t>::min() / timeBase.num) {
            return {pts * timeBase.num, timeBase.den};
        }
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                1'000'000'000};
    }

    domain::CaptureDeviceId deviceId_;
    std::string deviceName_;
    std::shared_ptr<capture::IAudioBlockSink> sink_;
    domain::SourceId id_;
    capture::AudioCaptureBlockAssembler assembler_;
    std::atomic_bool started_{};
    std::atomic_bool stopRequested_{};
    std::jthread worker_;
    std::thread stopThread_;
    mutable std::mutex mutex_;
    capture::CaptureStats stats_;
    bool terminalSent_{};
};

template <typename T>
class ComHandle final {
public:
    ~ComHandle() {
        if (value_) value_->Release();
    }
    ComHandle() = default;
    ComHandle(const ComHandle&) = delete;
    ComHandle& operator=(const ComHandle&) = delete;
    T* get() const noexcept { return value_; }
    T** put() noexcept { return &value_; }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    T* value_{};
};

struct EventDeleter final {
    void operator()(void* value) const noexcept {
        if (value) CloseHandle(static_cast<HANDLE>(value));
    }
};

struct WaveFormatDeleter final {
    void operator()(WAVEFORMATEX* value) const noexcept {
        if (value) CoTaskMemFree(value);
    }
};

bool isWaveSubtype(const WAVEFORMATEX& format, const GUID& subtype) noexcept {
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format)
                           .SubFormat,
                       subtype) != FALSE;
}

class SystemAudioSource final : public capture::IDeviceCaptureSource {
public:
    explicit SystemAudioSource(std::shared_ptr<capture::IAudioBlockSink> sink)
        : sink_(std::move(sink)),
          id_(domain::SourceId::create("windows/system-audio").value()) {}

    ~SystemAudioSource() override { static_cast<void>(stop()); }

    domain::SourceId id() const override { return id_; }
    std::string displayName() const override { return "Windows system audio"; }

    core::Result<void> start(const capture::CaptureConfig&) override {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Windows system audio is already started"};
        }
        stopRequested_.store(false, std::memory_order_release);
        worker_ = std::jthread{[this](std::stop_token) { run(); }};
        return core::ok();
    }

    core::Result<void> stop() override {
        stopRequested_.store(true, std::memory_order_release);
        const auto event = wakeEvent_.load(std::memory_order_acquire);
        if (event) SetEvent(event);
        if (stopThread_.joinable()) stopThread_.join();
        else if (worker_.joinable()) worker_.join();
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (stopThread_.joinable()) {
            if (completion) completion(core::AppError{
                core::ErrorCode::InvalidState,
                "Windows system-audio stop is already in progress"});
            return;
        }
        stopRequested_.store(true, std::memory_order_release);
        const auto event = wakeEvent_.load(std::memory_order_acquire);
        if (event) SetEvent(event);
        stopThread_ = std::thread{[this, completion = std::move(completion)]() mutable {
            if (worker_.joinable()) worker_.join();
            if (completion) completion(core::ok());
        }};
    }

    capture::CaptureStats stats() const noexcept override {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    void fail(core::AppError error) noexcept {
        std::shared_ptr<capture::IAudioBlockSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (terminalSent_ || stopRequested_.load()) return;
            terminalSent_ = true;
            sink = sink_;
        }
        if (sink) sink->onCaptureError(std::move(error));
    }

    void failHresult(std::string operation, HRESULT result) noexcept {
        fail(ioFailure(std::move(operation) + " failed (HRESULT " +
                       std::to_string(static_cast<unsigned long>(result)) + ")"));
    }

    void run() noexcept {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
            failHresult("WASAPI COM initialization", initialized);
            return;
        }
        runInitialized();
        if (uninitialize) CoUninitialize();
    }

    void runInitialized() noexcept {
        ComHandle<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.put()));
        if (FAILED(result)) {
            failHresult("WASAPI endpoint enumerator creation", result);
            return;
        }
        ComHandle<IMMDevice> endpoint;
        result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                      endpoint.put());
        if (FAILED(result)) {
            failHresult("WASAPI default speaker lookup", result);
            return;
        }
        ComHandle<IAudioClient> client;
        result = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.put()));
        if (FAILED(result)) {
            failHresult("WASAPI audio client activation", result);
            return;
        }
        WAVEFORMATEX* rawFormat = nullptr;
        result = client->GetMixFormat(&rawFormat);
        std::unique_ptr<WAVEFORMATEX, WaveFormatDeleter> format{rawFormat};
        if (FAILED(result) || !format) {
            failHresult("WASAPI mix-format lookup", result);
            return;
        }
        if (!supported(*format)) {
            fail(ioFailure("WASAPI default speaker uses an unsupported PCM format"));
            return;
        }
        std::unique_ptr<void, EventDeleter> event{
            CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        if (!event) {
            fail(ioFailure("WASAPI wake event creation failed"));
            return;
        }
        result = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_NOPERSIST,
            0, 0, format.get(), nullptr);
        if (FAILED(result)) {
            failHresult("WASAPI loopback initialization", result);
            return;
        }
        result = client->SetEventHandle(event.get());
        if (FAILED(result)) {
            failHresult("WASAPI wake-event binding", result);
            return;
        }
        ComHandle<IAudioCaptureClient> captureClient;
        result = client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(captureClient.put()));
        if (FAILED(result)) {
            failHresult("WASAPI loopback service lookup", result);
            return;
        }
        result = client->Start();
        if (FAILED(result)) {
            failHresult("WASAPI loopback start", result);
            return;
        }
        wakeEvent_.store(static_cast<HANDLE>(event.get()),
                         std::memory_order_release);
        if (sink_) sink_->onCaptureStarted();
        while (!stopRequested_.load(std::memory_order_acquire)) {
            const DWORD waited = WaitForSingleObject(event.get(), 250);
            if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) {
                fail(ioFailure("WASAPI loopback wait failed"));
                break;
            }
            if (stopRequested_.load(std::memory_order_acquire)) break;
            if (!drain(*captureClient.get(), *format)) break;
        }
        wakeEvent_.store(nullptr, std::memory_order_release);
        static_cast<void>(client->Stop());
    }

    static bool supported(const WAVEFORMATEX& format) noexcept {
        const bool floating = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                              isWaveSubtype(format, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const bool integer = format.wFormatTag == WAVE_FORMAT_PCM ||
                             isWaveSubtype(format, KSDATAFORMAT_SUBTYPE_PCM);
        return format.nChannels > 0 && format.nSamplesPerSec > 0 &&
               ((floating && format.wBitsPerSample == 32) ||
                (integer && (format.wBitsPerSample == 8 ||
                             format.wBitsPerSample == 16 ||
                             format.wBitsPerSample == 24 ||
                             format.wBitsPerSample == 32)));
    }

    bool drain(IAudioCaptureClient& client, const WAVEFORMATEX& format) noexcept {
        UINT32 packetFrames = 0;
        HRESULT result = client.GetNextPacketSize(&packetFrames);
        while (SUCCEEDED(result) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frameCount = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            result = client.GetBuffer(&data, &frameCount, &flags,
                                      &devicePosition, &qpcPosition);
            if (FAILED(result)) {
                failHresult("WASAPI loopback buffer read", result);
                return false;
            }
            deliver(data, frameCount, flags, qpcPosition, format);
            result = client.ReleaseBuffer(frameCount);
            if (FAILED(result)) {
                failHresult("WASAPI loopback buffer release", result);
                return false;
            }
            result = client.GetNextPacketSize(&packetFrames);
        }
        if (FAILED(result)) {
            failHresult("WASAPI loopback packet query", result);
            return false;
        }
        return true;
    }

    void deliver(const BYTE* data, UINT32 frameCount, DWORD flags,
                 UINT64 qpcPosition, const WAVEFORMATEX& format) noexcept {
        if (frameCount == 0) return;
        const auto channels = static_cast<std::uint32_t>(format.nChannels);
        const auto sampleCount = static_cast<std::size_t>(frameCount) * channels;
        auto samples = std::shared_ptr<float[]>{new float[sampleCount]};
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            samples[index] = silent ? 0.0F : sample(data, index, format);
        }
        auto assembled = assembler_.assemble(capture::NativeAudioBlock{
            .timestamp = {static_cast<std::int64_t>(qpcPosition), 10'000'000},
            .sampleRate = format.nSamplesPerSec,
            .channels = channels,
            .frameCount = frameCount,
            .sampleCount = sampleCount,
            .samples = std::move(samples)});
        if (!assembled.hasValue()) {
            std::lock_guard lock(mutex_);
            ++stats_.invalidFrames;
            return;
        }
        std::shared_ptr<capture::IAudioBlockSink> sink;
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_.load()) return;
            ++stats_.receivedFrames;
            sink = sink_;
        }
        if (sink) sink->onAudioBlock(std::move(assembled).value());
    }

    static float sample(const BYTE* data, std::size_t index,
                        const WAVEFORMATEX& format) noexcept {
        const bool floating = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                              isWaveSubtype(format, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const auto* value = data + index * (format.wBitsPerSample / 8U);
        if (floating) {
            float result = 0.0F;
            std::memcpy(&result, value, sizeof(result));
            return result;
        }
        switch (format.wBitsPerSample) {
            case 8:
                return (static_cast<float>(*value) - 128.0F) / 128.0F;
            case 16: {
                std::int16_t result{};
                std::memcpy(&result, value, sizeof(result));
                return static_cast<float>(result) / 32768.0F;
            }
            case 24: {
                std::int32_t result = static_cast<std::int32_t>(value[0]) |
                                      (static_cast<std::int32_t>(value[1]) << 8) |
                                      (static_cast<std::int32_t>(value[2]) << 16);
                if ((result & 0x00800000) != 0) result |= static_cast<std::int32_t>(0xFF000000);
                return static_cast<float>(result) / 8'388'608.0F;
            }
            case 32: {
                std::int32_t result{};
                std::memcpy(&result, value, sizeof(result));
                return static_cast<float>(result) / 2'147'483'648.0F;
            }
            default:
                return 0.0F;
        }
    }

    std::shared_ptr<capture::IAudioBlockSink> sink_;
    domain::SourceId id_;
    capture::AudioCaptureBlockAssembler assembler_;
    std::atomic_bool started_{};
    std::atomic_bool stopRequested_{};
    std::atomic<HANDLE> wakeEvent_{};
    std::jthread worker_;
    std::thread stopThread_;
    mutable std::mutex mutex_;
    capture::CaptureStats stats_;
    bool terminalSent_{};
};

class DeviceBackend final : public capture::IDeviceCaptureBackend {
public:
    explicit DeviceBackend(std::shared_ptr<CaptureRegistry> registry)
        : registry_(std::move(registry)) {}

    capture::MediaPermissionStatus permissionStatus(
        capture::CaptureDeviceKind) const noexcept override {
        return capture::MediaPermissionStatus::Granted;
    }

    void requestPermission(capture::CaptureDeviceKind,
                           PermissionCompletion completion) override {
        if (completion) completion(capture::MediaPermissionStatus::Granted);
    }

    core::Result<std::vector<capture::CaptureDeviceInfo>> devices(
        capture::CaptureDeviceKind kind) override {
        avdevice_register_all();
        const auto* input = av_find_input_format("dshow");
        if (!input) return ioFailure("audited FFmpeg dshow input is unavailable");
        AVDeviceInfoList* raw = nullptr;
        const int listed = avdevice_list_input_sources(input, nullptr, nullptr, &raw);
        std::unique_ptr<AVDeviceInfoList, DeviceListDeleter> list{raw};
        if (listed < 0 || !list) {
            return ioFailure("DirectShow device discovery failed: " +
                             ffmpegError(listed));
        }
        const AVMediaType wanted = kind == capture::CaptureDeviceKind::Camera
                                       ? AVMEDIA_TYPE_VIDEO
                                       : AVMEDIA_TYPE_AUDIO;
        std::vector<capture::CaptureDeviceInfo> result;
        std::unordered_map<std::string, std::string> mapping;
        for (int index = 0; index < list->nb_devices; ++index) {
            const auto* device = list->devices[index];
            if (!device || !device->device_name || !device->device_description) continue;
            bool matching = false;
            for (int mediaIndex = 0; mediaIndex < device->nb_media_types;
                 ++mediaIndex) {
                matching = matching || device->media_types[mediaIndex] == wanted;
            }
            if (!matching) continue;
            const std::string prefix =
                kind == capture::CaptureDeviceKind::Camera ? "camera:" : "microphone:";
            auto id = domain::CaptureDeviceId::create(
                prefix + std::to_string(index));
            if (!id.hasValue()) return id.error();
            auto info = capture::CaptureDeviceInfo::create(
                id.value(), kind, device->device_description,
                index == list->default_device);
            if (!info.hasValue()) return info.error();
            mapping.emplace(id.value().value(), device->device_name);
            result.push_back(std::move(info).value());
        }
        registry_->replaceDevices(kind, std::move(mapping));
        return result;
    }

    void setDeviceChangeHandler(DeviceChangeHandler handler) override {
        std::lock_guard lock(mutex_);
        changeHandler_ = std::move(handler);
    }

    core::Result<std::unique_ptr<capture::IDeviceCaptureSource>> createCamera(
        const domain::CaptureDeviceId& deviceId,
        std::shared_ptr<capture::IVideoFrameSink> sink) override {
        if (!sink) return invalid("camera sink is required");
        auto device = registry_->device(capture::CaptureDeviceKind::Camera,
                                        deviceId.value());
        if (!device.has_value()) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "selected Windows camera is unavailable"};
        }
        std::unique_ptr<capture::IDeviceCaptureSource> source =
            std::make_unique<CameraSource>(deviceId, std::move(*device),
                                           std::move(sink));
        return source;
    }

    core::Result<std::unique_ptr<capture::IDeviceCaptureSource>> createMicrophone(
        const domain::CaptureDeviceId& deviceId,
        std::shared_ptr<capture::IAudioBlockSink> sink) override {
        if (!sink) return invalid("microphone sink is required");
        auto device = registry_->device(capture::CaptureDeviceKind::Microphone,
                                        deviceId.value());
        if (!device.has_value()) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "selected Windows microphone is unavailable"};
        }
        std::unique_ptr<capture::IDeviceCaptureSource> source =
            std::make_unique<MicrophoneSource>(deviceId, std::move(*device),
                                               std::move(sink));
        return source;
    }

    core::Result<std::unique_ptr<capture::IDeviceCaptureSource>> createSystemAudio(
        std::shared_ptr<capture::IAudioBlockSink> sink) override {
        if (!sink) return invalid("system-audio sink is required");
        std::unique_ptr<capture::IDeviceCaptureSource> source =
            std::make_unique<SystemAudioSource>(std::move(sink));
        return source;
    }

private:
    std::shared_ptr<CaptureRegistry> registry_;
    std::mutex mutex_;
    DeviceChangeHandler changeHandler_;
};

}  // namespace

WindowsCaptureBackend makeWindowsCaptureBackend() {
    auto registry = std::make_shared<CaptureRegistry>();
    return {.screenPermission = std::make_unique<ScreenPermission>(),
            .screenDiscovery = std::make_unique<ScreenDiscovery>(registry),
            .screenSourceFactory = std::make_unique<ScreenFactory>(registry),
            .devices = std::make_unique<DeviceBackend>(std::move(registry))};
}

}  // namespace creator::ffmpeg_adapter::windows
