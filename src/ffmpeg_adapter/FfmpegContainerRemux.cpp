#include "ffmpeg_adapter/FfmpegContainerRemux.h"

#include "core/AppError.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

#include <string>
#include <vector>

namespace creator::ffmpeg_adapter {
namespace {

core::AppError ioError(std::string message) {
    return core::AppError{core::ErrorCode::IoFailure, std::move(message)};
}

// FFmpeg expects UTF-8 paths on Windows and resolves them itself.
std::string toUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string{encoded.begin(), encoded.end()};
}

}  // namespace

core::Result<void> remuxToMp4(const std::filesystem::path& source,
                              const std::filesystem::path& destination) {
    const std::string sourceUtf8 = toUtf8(source);
    const std::string destinationUtf8 = toUtf8(destination);

    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, sourceUtf8.c_str(), nullptr, nullptr) < 0 ||
        input == nullptr) {
        return ioError("remux: could not open the rendered source container");
    }
    if (avformat_find_stream_info(input, nullptr) < 0) {
        avformat_close_input(&input);
        return ioError("remux: could not read the source stream layout");
    }

    AVFormatContext* output = nullptr;
    if (avformat_alloc_output_context2(&output, nullptr, "mp4",
                                       destinationUtf8.c_str()) < 0 ||
        output == nullptr) {
        avformat_close_input(&input);
        return ioError("remux: could not allocate the MP4 output");
    }

    const auto cleanup = [&input, &output]() {
        if (output != nullptr) {
            if ((output->oformat->flags & AVFMT_NOFILE) == 0 &&
                output->pb != nullptr) {
                avio_closep(&output->pb);
            }
            avformat_free_context(output);
            output = nullptr;
        }
        if (input != nullptr) {
            avformat_close_input(&input);
        }
    };

    // Copy every audio/video stream's parameters into the output verbatim.
    std::vector<int> streamMap(input->nb_streams, -1);
    int outputStreamCount = 0;
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        AVStream* inStream = input->streams[index];
        const auto type = inStream->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* outStream = avformat_new_stream(output, nullptr);
        if (outStream == nullptr) {
            cleanup();
            return ioError("remux: could not create an output stream");
        }
        if (avcodec_parameters_copy(outStream->codecpar, inStream->codecpar) <
            0) {
            cleanup();
            return ioError("remux: could not copy stream codec parameters");
        }
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;
        streamMap[index] = outputStreamCount++;
    }
    if (outputStreamCount == 0) {
        cleanup();
        return ioError("remux: source has no audio or video streams");
    }

    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        if (avio_open(&output->pb, destinationUtf8.c_str(), AVIO_FLAG_WRITE) <
            0) {
            cleanup();
            return ioError("remux: could not open the MP4 destination");
        }
    }

    AVDictionary* muxerOptions = nullptr;
    av_dict_set(&muxerOptions, "movflags", "+faststart", 0);
    const int header = avformat_write_header(output, &muxerOptions);
    av_dict_free(&muxerOptions);
    if (header < 0) {
        cleanup();
        return ioError("remux: could not write the MP4 header");
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        cleanup();
        return ioError("remux: could not allocate a packet");
    }

    while (av_read_frame(input, packet) >= 0) {
        const int sourceIndex = packet->stream_index;
        const int mappedIndex =
            (sourceIndex >= 0 &&
             sourceIndex < static_cast<int>(streamMap.size()))
                ? streamMap[sourceIndex]
                : -1;
        if (mappedIndex < 0) {
            av_packet_unref(packet);
            continue;
        }
        AVStream* inStream = input->streams[sourceIndex];
        AVStream* outStream = output->streams[mappedIndex];
        packet->stream_index = mappedIndex;
        av_packet_rescale_ts(packet, inStream->time_base, outStream->time_base);
        packet->pos = -1;
        if (av_interleaved_write_frame(output, packet) < 0) {
            av_packet_unref(packet);
            av_packet_free(&packet);
            cleanup();
            return ioError("remux: could not write a media packet");
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    if (av_write_trailer(output) < 0) {
        cleanup();
        return ioError("remux: could not finalize the MP4 trailer");
    }
    cleanup();
    return core::ok();
}

}  // namespace creator::ffmpeg_adapter
