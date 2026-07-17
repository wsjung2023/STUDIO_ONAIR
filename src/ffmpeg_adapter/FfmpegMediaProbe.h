#pragma once

#include "media/IMediaProbe.h"

namespace creator::ffmpeg_adapter {

class FfmpegMediaProbe final : public media::IMediaProbe {
public:
    [[nodiscard]] core::Result<media::MediaProbeResult> probe(
        const std::filesystem::path& packageRoot,
        const std::filesystem::path& relativePath) override;
};

}  // namespace creator::ffmpeg_adapter
