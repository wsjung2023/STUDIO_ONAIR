#pragma once

#include "core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace creator::ffmpeg_adapter {

struct EncoderCapability final {
    std::string name;
    bool available{false};
};

struct FfmpegBuildInfo final {
    std::uint32_t avcodecVersion{0};
    std::uint32_t avformatVersion{0};
    std::uint32_t swresampleVersion{0};
    std::uint32_t swscaleVersion{0};
    std::string configuration;
    std::string license;
    std::vector<EncoderCapability> encoders;
};

[[nodiscard]] core::Result<FfmpegBuildInfo> probeFfmpegCapabilities();

}  // namespace creator::ffmpeg_adapter
