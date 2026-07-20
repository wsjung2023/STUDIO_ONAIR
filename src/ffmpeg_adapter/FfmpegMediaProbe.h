#pragma once

#include "core/Timebase.h"
#include "media/IMediaProbe.h"

#include <filesystem>
#include <vector>

namespace creator::ffmpeg_adapter {

struct FfmpegConcatEntry final {
    std::filesystem::path path;
    core::DurationNs duration;

    friend bool operator==(const FfmpegConcatEntry&,
                           const FfmpegConcatEntry&) = default;
};

/// Parses the exact ffconcat grammar emitted by the recorder reconciler and
/// resolves every entry under the manifest's package-root directory. Redirected,
/// hard-linked, absolute, and escaping entries are rejected before any decoder
/// receives a path.
[[nodiscard]] core::Result<std::vector<FfmpegConcatEntry>>
readValidatedFfmpegConcat(const std::filesystem::path& manifestPath);

class FfmpegMediaProbe final : public media::IMediaProbe {
public:
    [[nodiscard]] core::Result<media::MediaProbeResult> probe(
        const std::filesystem::path& packageRoot,
        const std::filesystem::path& relativePath) override;
};

}  // namespace creator::ffmpeg_adapter
