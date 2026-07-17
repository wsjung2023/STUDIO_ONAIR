#pragma once

#include "core/Result.h"
#include "core/Timebase.h"

#include <filesystem>

namespace creator::ffmpeg_adapter::detail {

[[nodiscard]] core::Result<core::DurationNs> probeEncodedDuration(
    const std::filesystem::path& path);

}  // namespace creator::ffmpeg_adapter::detail
