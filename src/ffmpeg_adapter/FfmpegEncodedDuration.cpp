#include "ffmpeg_adapter/FfmpegEncodedDuration.h"

#include "core/AppError.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

namespace creator::ffmpeg_adapter::detail {
namespace {

struct FormatCloser final {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) avformat_close_input(&context);
    }
};

std::string pathUrl(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

}  // namespace

core::Result<core::DurationNs> probeEncodedDuration(
    const std::filesystem::path& path) {
    AVFormatContext* opened = nullptr;
    const auto url = pathUrl(path);
    if (avformat_open_input(&opened, url.c_str(), nullptr, nullptr) < 0) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "Encoded segment duration could not be opened"};
    }
    std::unique_ptr<AVFormatContext, FormatCloser> context{opened};
    if (avformat_find_stream_info(context.get(), nullptr) < 0) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "Encoded segment duration could not be read"};
    }

    constexpr AVRational nanoseconds{1, 1'000'000'000};
    std::int64_t duration{};
    if (context->duration > 0 && context->duration != AV_NOPTS_VALUE) {
        duration = av_rescale_q_rnd(
            context->duration, AV_TIME_BASE_Q, nanoseconds,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    }
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        const auto* stream = context->streams[index];
        if (stream == nullptr || stream->duration <= 0 ||
            stream->duration == AV_NOPTS_VALUE || stream->time_base.num <= 0 ||
            stream->time_base.den <= 0) {
            continue;
        }
        const auto streamDuration = av_rescale_q_rnd(
            stream->duration, stream->time_base, nanoseconds,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        duration = std::max(duration, streamDuration);
    }
    if (duration <= 0 || duration == std::numeric_limits<std::int64_t>::max()) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "Encoded segment duration is missing or invalid"};
    }
    return core::DurationNs{duration};
}

}  // namespace creator::ffmpeg_adapter::detail
