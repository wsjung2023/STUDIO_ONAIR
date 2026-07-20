#pragma once

#include "core/Result.h"
#include "platform_release/ReleaseManifest.h"

#include <filesystem>

namespace creator::platform_release {

class ReleaseManifestStore final {
public:
    [[nodiscard]] core::Result<void> write(const std::filesystem::path& manifestPath,
                                            const std::filesystem::path& artifactRoot,
                                            const ReleaseManifest& manifest) const;
    [[nodiscard]] core::Result<ReleaseManifest> read(const std::filesystem::path& manifestPath,
                                                      const std::filesystem::path& artifactRoot) const;
};

}  // namespace creator::platform_release
