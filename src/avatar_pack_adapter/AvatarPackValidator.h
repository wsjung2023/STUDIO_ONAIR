#pragma once

#include "avatar/AvatarAssetManifest.h"
#include "core/Result.h"

#include <sodium.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace creator::avatar_pack_adapter {

struct TrustedAvatarKey final {
    std::string keyId;
    std::array<std::byte, crypto_sign_PUBLICKEYBYTES> publicKey{};
};

struct ValidatedAvatarPack final {
    avatar::AvatarAssetManifest manifest;
    /// Owned by the successful caller, which must delete this tree after use.
    std::filesystem::path stagingRoot;
};

/// Preflights, authenticates, and transactionally stages one avatar pack.
class AvatarPackValidator final {
public:
    AvatarPackValidator(std::vector<TrustedAvatarKey> trustedKeys,
                        std::filesystem::path stagingParent);

    [[nodiscard]] core::Result<ValidatedAvatarPack> validateAndExtract(
        const std::filesystem::path& packagePath) const;

private:
    std::vector<TrustedAvatarKey> trustedKeys_;
    std::filesystem::path stagingParent_;
};

}  // namespace creator::avatar_pack_adapter
