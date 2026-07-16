#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace creator::domain {

enum class MediaKind { Video, Audio, Image };
enum class AssetAvailability { Available, Offline };

struct VideoAssetMetadata final {
    std::int32_t width;
    std::int32_t height;
    core::FrameRate frameRate;

    friend bool operator==(const VideoAssetMetadata&, const VideoAssetMetadata&) = default;
};

struct AudioAssetMetadata final {
    std::int32_t sampleRate;
    std::int32_t channels;

    friend bool operator==(const AudioAssetMetadata&, const AudioAssetMetadata&) = default;
};

class MediaAsset final {
public:
    [[nodiscard]] static core::Result<MediaAsset> create(
        AssetId id, MediaKind kind, std::string_view relativePath,
        core::DurationNs duration, std::optional<VideoAssetMetadata> video,
        std::optional<AudioAssetMetadata> audio, std::uint64_t fileSize,
        std::string fingerprint, AssetAvailability availability);

    [[nodiscard]] const AssetId& id() const noexcept { return id_; }
    [[nodiscard]] MediaKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& relativePath() const noexcept { return relativePath_; }
    [[nodiscard]] core::DurationNs duration() const noexcept { return duration_; }
    [[nodiscard]] const std::optional<VideoAssetMetadata>& video() const noexcept {
        return video_;
    }
    [[nodiscard]] const std::optional<AudioAssetMetadata>& audio() const noexcept {
        return audio_;
    }
    [[nodiscard]] std::uint64_t fileSize() const noexcept { return fileSize_; }
    [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }
    [[nodiscard]] AssetAvailability availability() const noexcept { return availability_; }

    friend bool operator==(const MediaAsset&, const MediaAsset&) = default;

private:
    MediaAsset(AssetId id, MediaKind kind, std::string relativePath,
               core::DurationNs duration, std::optional<VideoAssetMetadata> video,
               std::optional<AudioAssetMetadata> audio, std::uint64_t fileSize,
               std::string fingerprint, AssetAvailability availability)
        : id_(std::move(id)),
          kind_(kind),
          relativePath_(std::move(relativePath)),
          duration_(duration),
          video_(std::move(video)),
          audio_(std::move(audio)),
          fileSize_(fileSize),
          fingerprint_(std::move(fingerprint)),
          availability_(availability) {}

    AssetId id_;
    MediaKind kind_;
    std::string relativePath_;
    core::DurationNs duration_;
    std::optional<VideoAssetMetadata> video_;
    std::optional<AudioAssetMetadata> audio_;
    std::uint64_t fileSize_;
    std::string fingerprint_;
    AssetAvailability availability_;
};

}  // namespace creator::domain
