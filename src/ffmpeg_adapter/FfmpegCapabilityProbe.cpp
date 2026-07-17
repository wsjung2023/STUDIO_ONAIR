#include "ffmpeg_adapter/FfmpegCapabilityProbe.h"

#include "core/AppError.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <string>

namespace creator::ffmpeg_adapter {

core::Result<FfmpegBuildInfo> probeFfmpegCapabilities() {
    const char* configurationValue = avcodec_configuration();
    const char* licenseValue = avcodec_license();
    if (configurationValue == nullptr || licenseValue == nullptr) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "FFmpeg did not expose build audit information"};
    }

    std::string configuration{configurationValue};
    std::string license{licenseValue};
    if (configuration.find("--enable-gpl") != std::string::npos ||
        configuration.find("--enable-nonfree") != std::string::npos ||
        configuration.find("--enable-shared") == std::string::npos ||
        license.find("LGPL") == std::string::npos) {
        return core::AppError{core::ErrorCode::UnsupportedVersion,
                              "FFmpeg runtime does not match the approved dynamic LGPL configuration"};
    }

    constexpr std::array encoderNames{
        "h264_videotoolbox", "h264_mf", "h264_nvenc", "h264_qsv", "mpeg4",
        "aac_mf", "aac"};
    std::vector<EncoderCapability> encoders;
    encoders.reserve(encoderNames.size());
    for (const char* name : encoderNames) {
        encoders.push_back({name, avcodec_find_encoder_by_name(name) != nullptr});
    }

    return FfmpegBuildInfo{
        .avcodecVersion = avcodec_version(),
        .avformatVersion = avformat_version(),
        .swresampleVersion = swresample_version(),
        .swscaleVersion = swscale_version(),
        .configuration = std::move(configuration),
        .license = std::move(license),
        .encoders = std::move(encoders),
    };
}

}  // namespace creator::ffmpeg_adapter
