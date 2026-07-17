#include "mlt_adapter/ExportEncoderProbe.h"

#include "core/AppError.h"
#include "ffmpeg_adapter/FfmpegCapabilityProbe.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "mlt_adapter/MltEditEngine.h"

#include <mlt++/MltConsumer.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace creator::mlt_adapter {
namespace {

std::string utf8Path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string{encoded.begin(), encoded.end()};
}

const std::array<ExportEncoderCandidate, 4>& candidates() {
    static const std::array<ExportEncoderCandidate, 4> value{{
        {.id = "h264_nvenc", .videoCodec = "h264_nvenc", .hardware = true},
        {.id = "h264_qsv", .videoCodec = "h264_qsv", .hardware = true},
        {.id = "h264_mf_hw",
         .videoCodec = "h264_mf",
         .hardware = true,
         .forceMediaFoundationHardware = true},
        {.id = "h264_mf_sw", .videoCodec = "h264_mf", .hardware = false},
    }};
    return value;
}

core::Result<void> renderProbe(const std::filesystem::path& output,
                               const ExportEncoderCandidate& candidate,
                               const edit_engine::RenderPreset& preset) {
    Mlt::Profile profile;
    if (!profile.is_valid()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "MLT could not create an export profile"};
    }
    profile.set_explicit(1);
    profile.set_width(static_cast<int>(preset.width()));
    profile.set_height(static_cast<int>(preset.height()));
    profile.set_frame_rate(static_cast<int>(preset.frameRate().numerator()),
                           static_cast<int>(preset.frameRate().denominator()));
    profile.set_sample_aspect(1, 1);
    profile.set_display_aspect(static_cast<int>(preset.width()),
                               static_cast<int>(preset.height()));
    profile.set_progressive(1);
    profile.set_colorspace(709);

    Mlt::Producer producer(profile, "color", "black");
    if (!producer.is_valid()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "MLT color producer is unavailable"};
    }
    producer.set_in_and_out(0, 2);

    const auto encodedOutput = utf8Path(output);
    Mlt::Consumer consumer(profile, "avformat", encodedOutput.c_str());
    if (!consumer.is_valid()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "MLT avformat consumer is unavailable"};
    }
    consumer.set("real_time", -1);
    consumer.set("an", 1);
    consumer.set("f", "mp4");
    consumer.set("vcodec", candidate.videoCodec.c_str());
    consumer.set("pix_fmt",
                 candidate.videoCodec == "h264_mf" ? "nv12" : "yuv420p");
    consumer.set("vb", static_cast<std::int64_t>(preset.videoBitrate()));
    consumer.set("movflags", "+faststart");
    if (candidate.videoCodec == "h264_mf") {
        consumer.set("hw_encoding",
                     candidate.forceMediaFoundationHardware ? 1 : 0);
    }
    if (consumer.connect(producer) != 0) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "MLT could not connect the encoder preflight graph"};
    }
    const int runResult = consumer.run();
    if (runResult != 0) {
        std::ostringstream message;
        message << "MLT avformat preflight failed with code " << runResult;
        if (const char* detail = consumer.get("error"); detail && *detail) {
            message << ": " << detail;
        }
        return core::AppError{core::ErrorCode::InvalidState, message.str()};
    }
    std::error_code fileError;
    const auto size = std::filesystem::file_size(output, fileError);
    if (fileError || size == 0) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "encoder preflight did not publish an MP4"};
    }
    ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    auto inspected = mediaProbe.probe(output.parent_path(), output.filename());
    if (!inspected.hasValue()) return inspected.error();
    if (inspected.value().codecName != "h264" ||
        !inspected.value().video.has_value() ||
        inspected.value().video->width != static_cast<int>(preset.width()) ||
        inspected.value().video->height != static_cast<int>(preset.height())) {
        return core::AppError{
            core::ErrorCode::InvalidState,
            "encoder preflight output is not the requested H.264 profile"};
    }
    return core::ok();
}

}  // namespace

core::Result<ExportEncoderSelection> ExportEncoderProbe::select(
    const edit_engine::RenderPreset& preset, const Attempt& attempt) {
    if (!attempt) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "encoder preflight attempt is required"};
    }
    std::vector<ExportEncoderAttempt> attempts;
    for (const auto& candidate : candidates()) {
        auto result = attempt(candidate, preset);
        if (result.hasValue()) {
            attempts.push_back({candidate, true, {}});
            return ExportEncoderSelection{candidate, std::move(attempts)};
        }
        attempts.push_back({candidate, false, result.error().message()});
    }
    std::ostringstream message;
    message << "no H.264 encoder passed physical preflight";
    for (const auto& item : attempts) {
        message << "; " << item.candidate.id << ": " << item.diagnostic;
    }
    return core::AppError{core::ErrorCode::InvalidState, message.str()};
}

core::Result<ExportEncoderSelection> ExportEncoderProbe::probe(
    const std::filesystem::path& runtimeRoot,
    const std::filesystem::path& scratchDirectory,
    const edit_engine::RenderPreset& preset) {
    auto initialized = MltEditEngine::initializeRuntime(runtimeRoot);
    if (!initialized.hasValue()) return initialized.error();
    auto capabilities = ffmpeg_adapter::probeFfmpegCapabilities();
    if (!capabilities.hasValue()) return capabilities.error();
    const auto isAvailable = [&](const std::string& name) {
        return std::any_of(capabilities.value().encoders.begin(),
                           capabilities.value().encoders.end(),
                           [&](const ffmpeg_adapter::EncoderCapability& value) {
                               return value.name == name && value.available;
                           });
    };
    if (!isAvailable("aac")) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "audited FFmpeg runtime has no AAC encoder"};
    }
    std::error_code directoryError;
    std::filesystem::create_directories(scratchDirectory, directoryError);
    if (directoryError) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "encoder preflight scratch directory is unavailable"};
    }
    static std::mutex renderMutex;
    std::lock_guard lock(renderMutex);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return select(preset, [&](const ExportEncoderCandidate& candidate,
                              const edit_engine::RenderPreset& value) {
        if (!isAvailable(candidate.videoCodec)) {
            return core::Result<void>{core::AppError{
                core::ErrorCode::InvalidState,
                candidate.videoCodec + " is absent from audited FFmpeg runtime"}};
        }
        if (candidate.forceMediaFoundationHardware) {
            return core::Result<void>{core::AppError{
                core::ErrorCode::InvalidState,
                "h264_mf hardware encoding requires a D3D11 frame pipeline"}};
        }
        const auto output = scratchDirectory /
                            ("encoder-preflight-" + candidate.id + "-" +
                             std::to_string(nonce) + ".mp4");
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        auto result = renderProbe(output, candidate, value);
        std::filesystem::remove(output, ignored);
        return result;
    });
}

}  // namespace creator::mlt_adapter
