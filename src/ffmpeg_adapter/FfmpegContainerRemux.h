#pragma once

#include "core/Result.h"

#include <filesystem>

namespace creator::ffmpeg_adapter {

// Stream-copies (no re-encode) the audio + video tracks of a rendered container
// into an MP4 at `destination`.
//
// The export path renders an audio-safe Matroska (the aac_mf encoder muxes
// cleanly into Matroska but the FFmpeg mp4 muxer rejects it for not exposing a
// frame_size, while the native "aac" encoder rejects the mixed program with a
// spurious "NaN/+-Inf" error). Remuxing the already-encoded AAC/H.264 streams
// into MP4 keeps the audio without a lossy, failure-prone re-encode.
[[nodiscard]] core::Result<void> remuxToMp4(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

}  // namespace creator::ffmpeg_adapter
