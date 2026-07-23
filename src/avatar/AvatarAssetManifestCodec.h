#pragma once

#include "avatar/AvatarAssetManifest.h"

#include <filesystem>

#include <nlohmann/json.hpp>

namespace creator::avatar {

/// Converts validated avatar asset manifests to and from canonical JSON.
class AvatarAssetManifestCodec final {
public:
    [[nodiscard]] nlohmann::json toJson(const AvatarAssetManifest& manifest) const;
    [[nodiscard]] core::Result<AvatarAssetManifest> fromJson(
        const nlohmann::json& document) const;
    [[nodiscard]] core::Result<AvatarAssetManifest> load(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> save(const std::filesystem::path& path,
                                          const AvatarAssetManifest& manifest) const;
};

}  // namespace creator::avatar
