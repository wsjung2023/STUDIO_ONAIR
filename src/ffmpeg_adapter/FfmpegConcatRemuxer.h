#pragma once

#include "core/Result.h"

#include <filesystem>

namespace creator::ffmpeg_adapter {

/// Materializes a validated recorder ffconcat manifest as one derived Matroska
/// file using packet-copy remuxing. The immutable segment files remain the
/// source of truth; the returned file is a replaceable editing cache.
[[nodiscard]] core::Result<std::filesystem::path>
materializeFfmpegConcatForEditing(
    const std::filesystem::path& manifestPath);

}  // namespace creator::ffmpeg_adapter
