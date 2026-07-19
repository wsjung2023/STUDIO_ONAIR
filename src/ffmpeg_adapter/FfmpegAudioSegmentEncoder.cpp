#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"

#include "core/AppError.h"
#include "ffmpeg_adapter/FfmpegEncodedDuration.h"
#include "sync/AudioRateCompensator.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

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

}  // namespace

class FfmpegAudioSegmentEncoder::Impl final {
public:
    explicit Impl(AudioEncoderOptions options) : options_(options) {}
    ~Impl() { abort(); }

    core::Result<void> start(const recorder::SegmentEncodeConfig& config) {
        if (started_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "FFmpeg audio segment is already started"};
        }
        if (config.track.mediaKind() != recorder::TrackMediaKind::Audio ||
            config.partPath.empty() || options_.bitRate <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "FFmpeg audio segment configuration is invalid"};
        }
        config_ = config;
        rateCompensator_ = {};
        started_ = true;
        return core::ok();
    }

    core::Result<void> accept(const media::AudioBlock& block) {
        if (!started_ || !config_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "FFmpeg audio segment has not been started"};
        }
        if (block.sampleRate == 0 || block.channels == 0 || block.frameCount == 0 ||
            block.frameCount > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            !block.samples || block.timestamp < config_->startTime ||
            !std::isfinite(block.sampleRateRatio) || block.sampleRateRatio < 0.999 ||
            block.sampleRateRatio > 1.001) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "FFmpeg audio block is invalid"};
        }
        if (!formatContext_) {
            if (auto opened = open(block.sampleRate, block.channels); !opened.hasValue()) {
                return opened.error();
            }
        }
        if (block.sampleRate != sampleRate_ || block.channels != channels_) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Audio format changed inside one segment"};
        }

        const auto compensation =
            rateCompensator_.next(block.frameCount, block.sampleRateRatio);
        if (!compensation.hasValue()) return compensation.error();

        constexpr AVRational nanoseconds{1, 1'000'000'000};
        std::int64_t inputPts = av_rescale_q(
            (block.timestamp - config_->startTime).count(), nanoseconds,
            codecContext_->time_base);
        if (expectedInputPts_) {
            const auto difference = inputPts - *expectedInputPts_;
            if (difference < -1) {
                return core::AppError{core::ErrorCode::InvalidArgument,
                                      "Audio block overlaps an earlier project timestamp"};
            }
            if (std::llabs(difference) <= 1) {
                inputPts = *expectedInputPts_;
            } else {
                if (av_audio_fifo_size(fifo_) > 0) {
                    if (auto partial = encodeFromFifo(av_audio_fifo_size(fifo_), true);
                        !partial.hasValue()) {
                        return partial.error();
                    }
                }
                fifoStartPts_ = inputPts;
            }
        }
        if (!fifoStartPts_) fifoStartPts_ = inputPts;

        AVFrame* converted = av_frame_alloc();
        if (converted == nullptr) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not allocate FFmpeg converted audio frame"};
        }
        if (compensation.value() != 0 || compensationActive_) {
            const int compensated = swr_set_compensation(
                resampler_, compensation.value(), static_cast<int>(block.frameCount));
            if (compensated < 0) {
                av_frame_free(&converted);
                return ffmpegError("Could not apply FFmpeg audio clock compensation",
                                   compensated);
            }
            compensationActive_ = true;
        }
        // swr_set_compensation invalidates earlier capacity estimates.
        const int capacity = swr_get_out_samples(resampler_, block.frameCount);
        if (capacity <= 0) {
            av_frame_free(&converted);
            return core::AppError{core::ErrorCode::IoFailure,
                                  "FFmpeg reported an invalid audio conversion capacity"};
        }
        converted->format = codecContext_->sample_fmt;
        converted->sample_rate = codecContext_->sample_rate;
        converted->nb_samples = capacity;
        int layout = av_channel_layout_copy(&converted->ch_layout,
                                            &codecContext_->ch_layout);
        int buffer = layout < 0 ? layout : av_frame_get_buffer(converted, 0);
        if (buffer < 0) {
            av_frame_free(&converted);
            return ffmpegError("Could not allocate FFmpeg converted audio planes", buffer);
        }
        const std::uint8_t* input[] = {
            reinterpret_cast<const std::uint8_t*>(block.samples.get())};
        const int convertedSamples =
            swr_convert(resampler_, converted->data, capacity, input, block.frameCount);
        if (convertedSamples < 0) {
            av_frame_free(&converted);
            return ffmpegError("Could not convert float recording audio", convertedSamples);
        }
        const int required = av_audio_fifo_size(fifo_) + convertedSamples;
        if (av_audio_fifo_realloc(fifo_, required) < 0 ||
            av_audio_fifo_write(fifo_, reinterpret_cast<void**>(converted->data),
                                convertedSamples) != convertedSamples) {
            av_frame_free(&converted);
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not buffer converted FFmpeg audio"};
        }
        av_frame_free(&converted);

        expectedInputPts_ = inputPts + block.frameCount;
        while (av_audio_fifo_size(fifo_) >= codecContext_->frame_size) {
            if (auto encoded = encodeFromFifo(codecContext_->frame_size, false);
                !encoded.hasValue()) {
                return encoded.error();
            }
        }
        return core::ok();
    }

    core::Result<recorder::EncodedSegment> finish(core::TimestampNs endTime) {
        if (!started_ || !config_ || !formatContext_ || !fifoStartPts_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Cannot finish an empty FFmpeg audio segment"};
        }
        if (endTime < config_->startTime) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Audio segment end precedes its start"};
        }
        const int remaining = av_audio_fifo_size(fifo_);
        if (remaining > 0) {
            if (auto encoded = encodeFromFifo(remaining, true); !encoded.hasValue()) {
                return encoded.error();
            }
        }
        const int sent = avcodec_send_frame(codecContext_, nullptr);
        if (sent < 0 && sent != AVERROR_EOF) {
            return ffmpegError("Could not flush FFmpeg AAC encoder", sent);
        }
        if (auto drained = drainPackets(); !drained.hasValue()) return drained.error();
        const int trailer = av_write_trailer(formatContext_);
        if (trailer < 0) return ffmpegError("Could not write Matroska audio trailer", trailer);

        const auto path = config_->partPath;
        const auto startTime = config_->startTime;
        closeMedia();
        std::error_code fileError;
        const auto bytes = std::filesystem::file_size(path, fileError);
        if (fileError || bytes == 0) {
            started_ = false;
            config_.reset();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "FFmpeg audio segment file is empty or unreadable"};
        }
        auto physicalDuration = detail::probeEncodedDuration(path);
        if (!physicalDuration.hasValue()) {
            started_ = false;
            config_.reset();
            return physicalDuration.error();
        }
        const auto publishedDuration =
            std::min(endTime - startTime, physicalDuration.value());
        started_ = false;
        config_.reset();
        return recorder::EncodedSegment{.endTime = startTime + publishedDuration,
                                        .bytesWritten = bytes,
                                        .codecName = "aac"};
    }

    void abort() noexcept {
        closeMedia();
        started_ = false;
        config_.reset();
    }

private:
    core::Result<void> open(std::uint32_t sampleRate, std::uint32_t channels) {
        if (sampleRate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            channels > 64) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Audio format exceeds FFmpeg limits"};
        }
        const auto url = pathUrl(config_->partPath);
        int allocated = avformat_alloc_output_context2(&formatContext_, nullptr, "matroska",
                                                        url.c_str());
        if (allocated < 0 || formatContext_ == nullptr) {
            closeMedia();
            return ffmpegError("Could not create Matroska audio output", allocated);
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("aac");
        if (codec == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::UnsupportedVersion,
                                  "Audited FFmpeg runtime has no AAC encoder"};
        }
        codecContext_ = avcodec_alloc_context3(codec);
        if (codecContext_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not allocate FFmpeg AAC context"};
        }
        codecContext_->codec_type = AVMEDIA_TYPE_AUDIO;
        codecContext_->codec_id = codec->id;
        codecContext_->sample_rate = static_cast<int>(sampleRate);
        codecContext_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        codecContext_->time_base = AVRational{1, static_cast<int>(sampleRate)};
        codecContext_->bit_rate = options_.bitRate;
        av_channel_layout_default(&codecContext_->ch_layout, static_cast<int>(channels));
        if ((formatContext_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            codecContext_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        const int opened = avcodec_open2(codecContext_, codec, nullptr);
        if (opened < 0) {
            closeMedia();
            return ffmpegError("Could not open FFmpeg AAC encoder", opened);
        }
        if (codecContext_->frame_size <= 0) {
            closeMedia();
            return core::AppError{core::ErrorCode::UnsupportedVersion,
                                  "FFmpeg AAC encoder has no fixed frame size"};
        }

        stream_ = avformat_new_stream(formatContext_, nullptr);
        if (stream_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not create Matroska audio stream"};
        }
        stream_->time_base = codecContext_->time_base;
        const int parameters =
            avcodec_parameters_from_context(stream_->codecpar, codecContext_);
        if (parameters < 0) {
            closeMedia();
            return ffmpegError("Could not copy FFmpeg audio parameters", parameters);
        }
        const int io = avio_open(&formatContext_->pb, url.c_str(), AVIO_FLAG_WRITE);
        if (io < 0) {
            closeMedia();
            return ffmpegError("Could not open FFmpeg audio segment file", io);
        }
        const int header = avformat_write_header(formatContext_, nullptr);
        if (header < 0) {
            closeMedia();
            return ffmpegError("Could not write Matroska audio header", header);
        }

        AVChannelLayout inputLayout{};
        av_channel_layout_default(&inputLayout, static_cast<int>(channels));
        const int resampler = swr_alloc_set_opts2(
            &resampler_, &codecContext_->ch_layout, codecContext_->sample_fmt,
            codecContext_->sample_rate, &inputLayout, AV_SAMPLE_FMT_FLT,
            static_cast<int>(sampleRate), 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        if (resampler < 0 || swr_init(resampler_) < 0) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not initialize FFmpeg audio resampler"};
        }
        fifo_ = av_audio_fifo_alloc(codecContext_->sample_fmt,
                                    codecContext_->ch_layout.nb_channels,
                                    codecContext_->frame_size * 2);
        packet_ = av_packet_alloc();
        if (fifo_ == nullptr || packet_ == nullptr) {
            closeMedia();
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not allocate FFmpeg audio buffers"};
        }
        sampleRate_ = sampleRate;
        channels_ = channels;
        return core::ok();
    }

    core::Result<void> encodeFromFifo(int sampleCount, bool padFinal) {
        const int outputCount = padFinal ? codecContext_->frame_size : sampleCount;
        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not allocate FFmpeg AAC frame"};
        }
        frame->format = codecContext_->sample_fmt;
        frame->sample_rate = codecContext_->sample_rate;
        frame->nb_samples = outputCount;
        int result = av_channel_layout_copy(&frame->ch_layout,
                                            &codecContext_->ch_layout);
        if (result >= 0) result = av_frame_get_buffer(frame, 0);
        if (result >= 0) {
            result = av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame->data),
                                        sampleCount);
            if (result == sampleCount && outputCount > sampleCount) {
                result = av_samples_set_silence(frame->data, sampleCount,
                                                outputCount - sampleCount,
                                                codecContext_->ch_layout.nb_channels,
                                                codecContext_->sample_fmt);
            }
        }
        if (result < 0 || (result != sampleCount && outputCount == sampleCount)) {
            av_frame_free(&frame);
            return core::AppError{core::ErrorCode::IoFailure,
                                  "Could not prepare buffered AAC samples"};
        }
        frame->pts = *fifoStartPts_;
        *fifoStartPts_ += sampleCount;
        int sent = avcodec_send_frame(codecContext_, frame);
        if (sent == AVERROR(EAGAIN)) {
            if (auto drained = drainPackets(); !drained.hasValue()) {
                av_frame_free(&frame);
                return drained.error();
            }
            sent = avcodec_send_frame(codecContext_, frame);
        }
        av_frame_free(&frame);
        if (sent < 0) return ffmpegError("FFmpeg rejected an AAC frame", sent);
        return drainPackets();
    }

    core::Result<void> drainPackets() {
        for (;;) {
            const int received = avcodec_receive_packet(codecContext_, packet_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return core::ok();
            if (received < 0) return ffmpegError("Could not receive FFmpeg AAC packet", received);
            av_packet_rescale_ts(packet_, codecContext_->time_base, stream_->time_base);
            packet_->stream_index = stream_->index;
            const int written = av_interleaved_write_frame(formatContext_, packet_);
            av_packet_unref(packet_);
            if (written < 0) return ffmpegError("Could not mux FFmpeg AAC packet", written);
        }
    }

    void closeMedia() noexcept {
        if (packet_ != nullptr) av_packet_free(&packet_);
        if (fifo_ != nullptr) av_audio_fifo_free(fifo_);
        fifo_ = nullptr;
        if (resampler_ != nullptr) swr_free(&resampler_);
        if (codecContext_ != nullptr) avcodec_free_context(&codecContext_);
        stream_ = nullptr;
        if (formatContext_ != nullptr) {
            if (formatContext_->pb != nullptr) avio_closep(&formatContext_->pb);
            avformat_free_context(formatContext_);
            formatContext_ = nullptr;
        }
        sampleRate_ = 0;
        channels_ = 0;
        fifoStartPts_.reset();
        expectedInputPts_.reset();
        compensationActive_ = false;
    }

    AudioEncoderOptions options_;
    std::optional<recorder::SegmentEncodeConfig> config_;
    bool started_{false};
    std::uint32_t sampleRate_{0};
    std::uint32_t channels_{0};
    std::optional<std::int64_t> fifoStartPts_;
    std::optional<std::int64_t> expectedInputPts_;
    synchronization::AudioRateCompensator rateCompensator_;
    bool compensationActive_{false};
    AVFormatContext* formatContext_{nullptr};
    AVCodecContext* codecContext_{nullptr};
    AVStream* stream_{nullptr};
    SwrContext* resampler_{nullptr};
    AVAudioFifo* fifo_{nullptr};
    AVPacket* packet_{nullptr};
};

FfmpegAudioSegmentEncoder::FfmpegAudioSegmentEncoder(AudioEncoderOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

FfmpegAudioSegmentEncoder::~FfmpegAudioSegmentEncoder() = default;

core::Result<void> FfmpegAudioSegmentEncoder::start(
    const recorder::SegmentEncodeConfig& config) {
    return impl_->start(config);
}

core::Result<void> FfmpegAudioSegmentEncoder::accept(const media::VideoFrame&) {
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "Video was submitted to an FFmpeg audio encoder"};
}

core::Result<void> FfmpegAudioSegmentEncoder::accept(const media::AudioBlock& block) {
    return impl_->accept(block);
}

core::Result<recorder::EncodedSegment> FfmpegAudioSegmentEncoder::finish(
    core::TimestampNs endTime) {
    return impl_->finish(endTime);
}

void FfmpegAudioSegmentEncoder::abort() noexcept {
    impl_->abort();
}

}  // namespace creator::ffmpeg_adapter
