#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/MediaAsset.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace creator::media {

class IMediaIdentityLease {
public:
    [[nodiscard]] virtual core::Result<void> verifyCurrentIdentity() const = 0;
    virtual ~IMediaIdentityLease() = default;
};

struct MediaProbeResult final {
    core::DurationNs duration;
    std::optional<domain::VideoAssetMetadata> video;
    std::optional<domain::AudioAssetMetadata> audio;
    std::string formatName;
    std::string codecName;
    std::uint64_t byteSize;
    std::string sha256;
    std::shared_ptr<const IMediaIdentityLease> identityLease;

    friend bool operator==(const MediaProbeResult& first,
                           const MediaProbeResult& second) {
        return first.duration == second.duration && first.video == second.video &&
               first.audio == second.audio &&
               first.formatName == second.formatName &&
               first.codecName == second.codecName &&
               first.byteSize == second.byteSize &&
               first.sha256 == second.sha256;
    }
};

class IMediaProbe {
public:
    [[nodiscard]] virtual core::Result<MediaProbeResult> probe(
        const std::filesystem::path& packageRoot,
        const std::filesystem::path& relativePath) = 0;
    virtual ~IMediaProbe() = default;
};

}  // namespace creator::media
