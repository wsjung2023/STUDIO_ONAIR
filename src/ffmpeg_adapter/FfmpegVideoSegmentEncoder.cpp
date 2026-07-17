#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"

#include "ffmpeg_adapter/FfmpegEncodedDuration.h"

#include "core/AppError.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace creator::ffmpeg_adapter {
namespace {

core::AppError ffmpegError(std::string operation, int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(code, message.data(), message.size());
    return {core::ErrorCode::IoFailure,
            std::move(operation) + ": " + std::string{message.data()}};
}

std::string pathUrl(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.string();
#endif
}

std::vector<std::string> defaultEncoders() {
#ifdef __APPLE__
    return {"h264_videotoolbox", "mpeg4"};
#elif defined(_WIN32)
    return {"h264_mf", "h264_nvenc", "h264_qsv", "mpeg4"};
#else
    return {"mpeg4"};
#endif
}

}  // namespace

class FfmpegVideoSegmentEncoder::Impl final {
public:
    Impl(std::unique_ptr<recorder::IVideoFrameMapper> mapper,
         VideoEncoderOptions options)
        : mapper_(std::move(mapper)), options_(std::move(options)) {}

    ~Impl() { abort(); }

    core::Result<void> start(const recorder::SegmentEncodeConfig& config) {
        if (started_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "FFmpeg video segment is already started"};
        }
        if (!mapper_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "FFmpeg video encoder has no frame mapper"};
        }
        if (config.track.mediaKind() != recorder::TrackMediaKind::Video ||
            config.partPath.empty() || options_.frameRateNumerator <= 0 ||
            options_.frameRateDenominator <= 0 || options_.bitRate <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "FFmpeg video segment configuration is invalid"};
        }
        if (options_.frameRateNumerator > std::numeric_limits<int>::max() ||
            options_.frameRateDenominator > std::numeric_limits<int>::max()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "FFmpeg video frame rate exceeds codec limits"};
        }
        config_ = config;
        started_ = true;
        frameCount_ = 0;
        return core::ok();
    }

    core::Result<void> accept(const media::VideoFrame& frame) {
        if (!started_ || !config_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "FFmpeg video segment has not been started"};
        }
        auto mapped = mapper_->map(frame);
        if (!mapped.hasValue()) return mapped.error();
        if (mapped.value().width == 0 || mapped.value().height == 0 ||
            mapped.value().rowBytes < mapped.value().width * 4ULL ||
            mapped.value().rowBytes > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            mapped.value().width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            mapped.value().height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            mapped.value().data == nullptr) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Mapped BGRA video frame is invalid"};
        }
        if (!formatContext_) {
            if (auto opened = open(mapped.value().width, mapped.value().height);
                !opened.hasValue()) {
                return opened.error();
            }
        }
        if (mapped.value().width != static_cast<std::uint32_t>(codecContext_->width) ||
            mapped.value().height != static_cast<std::uint32_t>(codecContext_->height)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Video dimensions changed inside one segment"};
        }
        if (frame.timestamp < config_->startTime) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Video frame timestamp precedes segment start"};
        }

        const int writable = av_frame_make_writable(outputFrame_);
        if (writable < 0) return ffmpegError("Could not reuse FFmpeg video frame", writable);
        const std::uint8_t* sourceData[] = {mapped.value().data};
        const int sourceStride[] = {static_cast<int>(mapped.value().rowBytes)};
        const int scaled = sws_scale(scaler_, sourceData, sourceStride, 0,
                                     static_cast<int>(mapped.value().height),
                                     outputFrame_->data, outputFrame_->linesize);
        if (scaled != codecContext_->height) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "FFmpeg could not convert the complete video frame"};
        }

        constexpr AVRational nanoseconds{1, 1'000'000'000};
        outputFrame_->pts = av_rescale_q((frame.timestamp - config_->startTime).count(),
                                        nanoseconds, codecContext_->time_base);
        outputFrame_->duration = av_rescale_q(
            1, av_inv_q(codecContext_->framerate), codecContext_->time_base);
        outputFrame_->pict_type = frameCount_ == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (frameCount_ == 0) outputFrame_->flags |= AV_FRAME_FLAG_KEY;
        else outputFrame_->flags &= ~AV_FRAME_FLAG_KEY;

        int sent = avcodec_send_frame(codecContext_, outputFrame_);
        if (sent == AVERROR(EAGAIN)) {
            if (auto drained = drainPackets(); !drained.hasValue()) return drained.error();
            sent = avcodec_send_frame(codecContext_, outputFrame_);
        }
        if (sent < 0) return ffmpegError("FFmpeg rejected a video frame", sent);
        if (auto drained = drainPackets(); !drained.hasValue()) return drained.error();
        ++frameCount_;
        return core::ok();
    }

    core::Result<recorder::EncodedSegment> finish(core::TimestampNs endTime) {
        if (!started_ || !config_ || !formatContext_ || frameCount_ == 0) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Cannot finish an empty FFmpeg video segment"};
        }
        if (endTime < config_->startTime) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Video segment end precedes its start"};
        }
        const int sent = avcodec_send_frame(codecContext_, nullptr);
        if (sent < 0 && sent != AVERROR_EOF) {
            return ffmpegError("Could not flush FFmpeg video encoder", sent);
        }
        if (auto drained = drainPackets(); !drained.hasValue()) return drained.error();
        const int trailer = av_write_trailer(formatContext_);
        if (trailer < 0) return ffmpegError("Could not write Matroska video trailer", trailer);

        const auto path = config_->partPath;
        const auto startTime = config_->startTime;
        const auto codecName = selectedEncoderName_;
        closeMedia();
        std::error_code fileError;
        const auto bytes = std::filesystem::file_size(path, fileError);
        if (fileError || bytes == 0) {
            started_ = false;
            config_.reset();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "FFmpeg video segment file is empty or unreadable"};
        }
        auto physicalDuration = detail::probeEncodedDuration(path);
        if (!physicalDuration.hasValue()) {
            if (frameCount_ != 1) {
                started_ = false;
                config_.reset();
                return physicalDuration.error();
            }
            constexpr AVRational nanoseconds{1, 1'000'000'000};
            physicalDuration = core::DurationNs{av_rescale_q(
                1,
                AVRational{static_cast<int>(options_.frameRateDenominator),
                           static_cast<int>(options_.frameRateNumerator)},
                nanoseconds)};
        }
        const auto publishedDuration =
            std::min(endTime - startTime, physicalDuration.value());
        started_ = false;
        config_.reset();
        return recorder::EncodedSegment{.endTime = startTime + publishedDuration,
                                        .bytesWritten = bytes,
                                        .codecName = codecName};
    }

    void abort() noexcept {
        closeMedia();
        started_ = false;
        config_.reset();
        frameCount_ = 0;
    }

private:
    core::Result<void> open(std::uint32_t width, std::uint32_t height) {
        const auto url = pathUrl(config_->partPath);
        int allocated = avformat_alloc_output_context2(&formatContext_, nullptr, "matroska",
                                                        url.c_str());
        if (allocated < 0 || formatContext_ == nullptr) {
            closeMedia();
            return ffmpegError("Could not create Matroska video output", allocated);
        }

        const auto candidates = options_.preferredEncoderNames.empty()
                                    ? defaultEncoders()
                                    : options_.preferredEncoderNames;
        std::string lastOpenError = "No requested FFmpeg video encoder is installed";
        for (const auto& name : candidates) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
            if (codec == nullptr) continue;
            AVCodecContext* candidate = avcodec_alloc_context3(codec);
            if (candidate == nullptr) {
                closeMedia();
                return core::AppError{core::ErrorCode::IoFailure,
                                      "Could not allocate FFmpeg video codec context"};
            }
            candidate->codec_type = AVMEDIA_TYPE_VIDEO;
            candidate->codec_id = codec->id;
            candidate->width = static_cast<int>(width);
            candidate->height = static_cast<int>(height);
            candidate->pix_fmt = AV_PIX_FMT_YUV420P;
            candidate->time_base = AVRational{1, 60'000};
            candidate->framerate = AVRational{
                static_cast<int>(options_.frameRateNumerator),
                static_cast<int>(options_.frameRateDenominator)};
            candidate->bit_rate = options_.bitRate;
            candidate->gop_size = std::max(
                1, static_cast<int>((options_.frameRateNumerator * 2) /
                                    options_.frameRateDenominator));
            candidate->max_b_frames = 0;
            if ((formatContext_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
                candidate->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            AVDictionary* codecOptions = nullptr;
            if (name == "h264_mf") av_dict_set(&codecOptions, "hw_encoding", "1", 0);
            const int opened = avcodec_open2(candidate, codec, &codecOptions);
            av_dict_free(&codecOptions);
            if (opened >= 0) {
                codecContext_ = candidate;
                selectedEncoderName_ = name;
                break;
            }
            lastOpenError = ffmpegError("Could not open FFmpeg encoder " + name, opened).message();
            avcodec_free_context(&candidate);
        }
        if (codecContext_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::UnsupportedVersion, std::move(lastOpenError)};
        }

        stream_ = avformat_new_stream(formatContext_, nullptr);
        if (stream_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not create Matroska video stream"};
        }
        stream_->time_base = codecContext_->time_base;
        const int parameters =
            avcodec_parameters_from_context(stream_->codecpar, codecContext_);
        if (parameters < 0) {
            closeMedia();
            return ffmpegError("Could not copy FFmpeg video parameters", parameters);
        }

        const int io = avio_open(&formatContext_->pb, url.c_str(), AVIO_FLAG_WRITE);
        if (io < 0) {
            closeMedia();
            return ffmpegError("Could not open FFmpeg video segment file", io);
        }
        const int header = avformat_write_header(formatContext_, nullptr);
        if (header < 0) {
            closeMedia();
            return ffmpegError("Could not write Matroska video header", header);
        }

        outputFrame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (outputFrame_ == nullptr || packet_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not allocate FFmpeg video frame or packet"};
        }
        outputFrame_->format = codecContext_->pix_fmt;
        outputFrame_->width = codecContext_->width;
        outputFrame_->height = codecContext_->height;
        const int buffer = av_frame_get_buffer(outputFrame_, 32);
        if (buffer < 0) {
            closeMedia();
            return ffmpegError("Could not allocate FFmpeg video planes", buffer);
        }
        scaler_ = sws_getContext(codecContext_->width, codecContext_->height,
                                 AV_PIX_FMT_BGRA, codecContext_->width,
                                 codecContext_->height, codecContext_->pix_fmt,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not create FFmpeg BGRA converter"};
        }
        return core::ok();
    }

    core::Result<void> drainPackets() {
        for (;;) {
            const int received = avcodec_receive_packet(codecContext_, packet_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return core::ok();
            if (received < 0) return ffmpegError("Could not receive FFmpeg video packet", received);
            av_packet_rescale_ts(packet_, codecContext_->time_base, stream_->time_base);
            packet_->stream_index = stream_->index;
            const int written = av_interleaved_write_frame(formatContext_, packet_);
            av_packet_unref(packet_);
            if (written < 0) return ffmpegError("Could not mux FFmpeg video packet", written);
        }
    }

    void closeMedia() noexcept {
        if (packet_ != nullptr) av_packet_free(&packet_);
        if (outputFrame_ != nullptr) av_frame_free(&outputFrame_);
        if (scaler_ != nullptr) sws_freeContext(scaler_);
        scaler_ = nullptr;
        if (codecContext_ != nullptr) avcodec_free_context(&codecContext_);
        stream_ = nullptr;
        if (formatContext_ != nullptr) {
            if (formatContext_->pb != nullptr) avio_closep(&formatContext_->pb);
            avformat_free_context(formatContext_);
            formatContext_ = nullptr;
        }
        selectedEncoderName_.clear();
    }

    std::unique_ptr<recorder::IVideoFrameMapper> mapper_;
    VideoEncoderOptions options_;
    std::optional<recorder::SegmentEncodeConfig> config_;
    bool started_{false};
    std::uint64_t frameCount_{0};
    std::string selectedEncoderName_;
    AVFormatContext* formatContext_{nullptr};
    AVCodecContext* codecContext_{nullptr};
    AVStream* stream_{nullptr};
    AVFrame* outputFrame_{nullptr};
    AVPacket* packet_{nullptr};
    SwsContext* scaler_{nullptr};
};

FfmpegVideoSegmentEncoder::FfmpegVideoSegmentEncoder(
    std::unique_ptr<recorder::IVideoFrameMapper> mapper, VideoEncoderOptions options)
    : impl_(std::make_unique<Impl>(std::move(mapper), std::move(options))) {}

FfmpegVideoSegmentEncoder::~FfmpegVideoSegmentEncoder() = default;

core::Result<void> FfmpegVideoSegmentEncoder::start(
    const recorder::SegmentEncodeConfig& config) {
    return impl_->start(config);
}

core::Result<void> FfmpegVideoSegmentEncoder::accept(const media::VideoFrame& frame) {
    return impl_->accept(frame);
}

core::Result<void> FfmpegVideoSegmentEncoder::accept(const media::AudioBlock&) {
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "Audio was submitted to an FFmpeg video encoder"};
}

core::Result<recorder::EncodedSegment> FfmpegVideoSegmentEncoder::finish(
    core::TimestampNs endTime) {
    return impl_->finish(endTime);
}

void FfmpegVideoSegmentEncoder::abort() noexcept {
    impl_->abort();
}

}  // namespace creator::ffmpeg_adapter
