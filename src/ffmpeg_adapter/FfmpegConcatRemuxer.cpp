#include "ffmpeg_adapter/FfmpegConcatRemuxer.h"

#include "core/AppError.h"
#include "core/Sha256.h"
#include "core/Uuid.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::ffmpeg_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError invalid(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message)};
}

AppError ioFailure(std::string message) {
    return {ErrorCode::IoFailure, std::move(message)};
}

AppError parseFailure(std::string message) {
    return {ErrorCode::ParseFailure, std::move(message)};
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string{encoded.begin(), encoded.end()};
}

class Dictionary final {
public:
    ~Dictionary() { av_dict_free(&value); }
    AVDictionary* value{};
};

class InputCloser final {
public:
    void operator()(AVFormatContext* context) const noexcept {
        avformat_close_input(&context);
    }
};

class OutputContext final {
public:
    explicit OutputContext(AVFormatContext* context) : context_(context) {}
    OutputContext(const OutputContext&) = delete;
    OutputContext& operator=(const OutputContext&) = delete;
    ~OutputContext() {
        if (context_ != nullptr) {
            if (context_->pb != nullptr) avio_closep(&context_->pb);
            avformat_free_context(context_);
        }
    }

    [[nodiscard]] AVFormatContext* get() const noexcept { return context_; }

private:
    AVFormatContext* context_{};
};

class PacketCloser final {
public:
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

class PartialArtifact final {
public:
    explicit PartialArtifact(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~PartialArtifact() {
        if (!published_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    void published() noexcept { published_ = true; }

private:
    std::filesystem::path path_;
    bool published_{};
};

Result<void> publishAtomically(const std::filesystem::path& partial,
                               const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(partial.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return ioFailure("derived editing media could not be atomically published");
    }
#else
    std::error_code error;
    std::filesystem::rename(partial, destination, error);
    if (error) {
        return ioFailure("derived editing media could not be atomically published: " +
                         error.message());
    }
#endif
    return core::ok();
}

Result<core::DurationNs> expectedDuration(
    const std::vector<FfmpegConcatEntry>& entries) {
    core::DurationNs total{};
    for (const auto& entry : entries) {
        if (entry.duration.count() <= 0 ||
            total.count() > std::numeric_limits<std::int64_t>::max() -
                                entry.duration.count()) {
            return invalid("concat duration exceeds the supported range");
        }
        total += entry.duration;
    }
    return total;
}

bool reusableCache(const std::filesystem::path& destination,
                   core::DurationNs expected) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(destination, error) || error) {
        return false;
    }
    FfmpegMediaProbe probe;
    auto inspected = probe.probe(destination.parent_path(),
                                 destination.filename());
    if (!inspected.hasValue()) return false;
    const auto delta = inspected.value().duration > expected
                           ? inspected.value().duration - expected
                           : expected - inspected.value().duration;
    return delta <= std::chrono::milliseconds{100} &&
           (inspected.value().video.has_value() !=
            inspected.value().audio.has_value());
}

Result<void> remux(const std::filesystem::path& manifest,
                   const std::filesystem::path& partial) {
    Dictionary inputOptions;
    if (av_dict_set(&inputOptions.value, "safe", "0", 0) < 0) {
        return ioFailure("FFmpeg concat options could not be allocated");
    }
    AVFormatContext* opened = nullptr;
    const auto inputPath = utf8Path(manifest);
    if (avformat_open_input(&opened, inputPath.c_str(), nullptr,
                            &inputOptions.value) < 0) {
        return parseFailure("validated concat media could not be opened");
    }
    std::unique_ptr<AVFormatContext, InputCloser> input{opened};
    // The concat demuxer's stream table is complete after read_header opens
    // the first recorder segment. avformat_find_stream_info() is harmful here:
    // for a long concat it scans every segment before remuxing and retains one
    // nested demuxer/IO graph per file, producing tens of thousands of Windows
    // handles. Packet copy below validates read failures while keeping only the
    // currently consumed segment open.
    if (input->nb_streams != 1 || input->streams[0] == nullptr ||
        input->streams[0]->codecpar == nullptr) {
        return parseFailure(
            "concat editing media must contain exactly one stream");
    }

    AVFormatContext* allocated = nullptr;
    const auto outputPath = utf8Path(partial);
    if (avformat_alloc_output_context2(&allocated, nullptr, "matroska",
                                       outputPath.c_str()) < 0 ||
        allocated == nullptr) {
        return ioFailure("FFmpeg Matroska output could not be allocated");
    }
    OutputContext output{allocated};
    AVStream* outputStream = avformat_new_stream(output.get(), nullptr);
    if (outputStream == nullptr ||
        avcodec_parameters_copy(outputStream->codecpar,
                                input->streams[0]->codecpar) < 0) {
        return ioFailure("FFmpeg stream metadata could not be copied");
    }
    outputStream->codecpar->codec_tag = 0;
    outputStream->time_base = input->streams[0]->time_base;
    if ((output.get()->oformat->flags & AVFMT_NOFILE) == 0 &&
        avio_open(&output.get()->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        return ioFailure("derived editing media could not be created");
    }
    if (avformat_write_header(output.get(), nullptr) < 0) {
        return ioFailure("derived editing media header could not be written");
    }

    std::unique_ptr<AVPacket, PacketCloser> packet{av_packet_alloc()};
    if (!packet) return ioFailure("FFmpeg packet could not be allocated");
    std::int64_t nextDts = AV_NOPTS_VALUE;
    for (;;) {
        const int read = av_read_frame(input.get(), packet.get());
        if (read == AVERROR_EOF) break;
        if (read < 0) {
            return parseFailure("concat packet stream ended unexpectedly");
        }
        if (packet->stream_index != 0) {
            av_packet_unref(packet.get());
            continue;
        }
        av_packet_rescale_ts(packet.get(), input->streams[0]->time_base,
                             outputStream->time_base);
        if (packet->dts != AV_NOPTS_VALUE) {
            if (nextDts != AV_NOPTS_VALUE && packet->dts < nextDts) {
                const auto duration =
                    std::max<std::int64_t>(packet->duration, 1);
                const auto packetEnd = packet->dts + duration;
                if (packetEnd <= nextDts) {
                    av_packet_unref(packet.get());
                    continue;
                }
                packet->duration = packetEnd - nextDts;
                packet->dts = nextDts;
                if (packet->pts != AV_NOPTS_VALUE && packet->pts < nextDts) {
                    packet->pts = nextDts;
                }
            }
            nextDts = packet->dts + std::max<std::int64_t>(packet->duration, 1);
        }
        packet->stream_index = outputStream->index;
        packet->pos = -1;
        if (av_interleaved_write_frame(output.get(), packet.get()) < 0) {
            av_packet_unref(packet.get());
            return ioFailure("derived editing media packet could not be written");
        }
        av_packet_unref(packet.get());
    }
    if (av_write_trailer(output.get()) < 0) {
        return ioFailure("derived editing media trailer could not be written");
    }
    return core::ok();
}

std::mutex& materializationMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

Result<std::filesystem::path> materializeFfmpegConcatForEditing(
    const std::filesystem::path& manifestPath) {
    if (manifestPath.extension() != ".ffconcat") {
        return invalid("editing materialization requires an ffconcat manifest");
    }
    auto entries = readValidatedFfmpegConcat(manifestPath);
    if (!entries.hasValue()) return entries.error();
    auto duration = expectedDuration(entries.value());
    if (!duration.hasValue()) return duration.error();
    auto manifestHash = core::sha256File(manifestPath);
    if (!manifestHash.hasValue()) return manifestHash.error();

    std::lock_guard lock(materializationMutex());
    const auto destination =
        manifestPath.parent_path() /
        (".creator-studio-mlt-cache-v2-" + manifestHash.value() + ".mkv");
    if (reusableCache(destination, duration.value())) return destination;

    const auto partial = manifestPath.parent_path() /
                         (".creator-studio-cache-part-" +
                          core::generateUuidV4() + ".mkv");
    PartialArtifact cleanup{partial};
    auto copied = remux(manifestPath, partial);
    if (!copied.hasValue()) return copied.error();
    {
        FfmpegMediaProbe probe;
        auto inspected = probe.probe(partial.parent_path(), partial.filename());
        if (!inspected.hasValue()) return inspected.error();
        const auto delta = inspected.value().duration > duration.value()
                               ? inspected.value().duration - duration.value()
                               : duration.value() - inspected.value().duration;
        if (delta > std::chrono::milliseconds{100} ||
            (inspected.value().video.has_value() ==
             inspected.value().audio.has_value())) {
            return parseFailure(
                "derived editing media failed stream or duration validation: "
                "expected=" +
                std::to_string(duration.value().count()) + "ns actual=" +
                std::to_string(inspected.value().duration.count()) + "ns");
        }
    }
    auto published = publishAtomically(partial, destination);
    if (!published.hasValue()) return published.error();
    cleanup.published();
    return destination;
}

}  // namespace creator::ffmpeg_adapter
