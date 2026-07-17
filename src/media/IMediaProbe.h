#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/MediaAsset.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace creator::media {

struct MediaProbeResult final {
    core::DurationNs duration;
    std::optional<domain::VideoAssetMetadata> video;
    std::optional<domain::AudioAssetMetadata> audio;
    std::string formatName;
    std::string codecName;
    std::uint64_t byteSize;
    std::string sha256;

    friend bool operator==(const MediaProbeResult&,
                           const MediaProbeResult&) = default;
};

class IMediaProbe {
public:
    [[nodiscard]] virtual core::Result<MediaProbeResult> probe(
        const std::filesystem::path& packageRoot,
        const std::filesystem::path& relativePath) = 0;
    virtual ~IMediaProbe() = default;
};

}  // namespace creator::media
