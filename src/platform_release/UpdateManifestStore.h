#pragma once

#include "core/Result.h"
#include "platform_release/UpdateManifest.h"

#include <cstddef>
#include <filesystem>
#include <span>

namespace creator::platform_release {

class UpdateManifestStore final {
public:
    [[nodiscard]] core::Result<void> write(
        const std::filesystem::path& path, const UpdateManifest& manifest,
        std::span<const std::byte> signature) const;
    [[nodiscard]] core::Result<UpdateManifest> loadVerified(
        const std::filesystem::path& path,
        const IUpdateSignatureVerifier& verifier) const;
};

}  // namespace creator::platform_release
