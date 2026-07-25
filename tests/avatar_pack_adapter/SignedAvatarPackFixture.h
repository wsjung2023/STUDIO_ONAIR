#pragma once

#include "avatar_pack_adapter/AvatarPackValidator.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace creator::avatar_pack_adapter::test {

struct SignedPackPayload final {
    std::string path{"payload/model.bin"};
    std::vector<std::uint8_t> contents;
};

struct SignedPackOptions final {
    std::string packageId{"vendor.foundation"};
    std::string packageVersion{"1.0.0"};
    std::string assetId{"core.body.humanoid"};
    std::string assetVersion{"1.0.0"};
    std::vector<SignedPackPayload> payloads;
    std::optional<std::uint64_t> declaredPayloadBytes;
    std::optional<std::string> rawManifestPackageVersion;
    bool corruptSignature{false};
};

/// Builds deterministic-shape packs signed by an ephemeral test-only key.
class SignedAvatarPackFixture final {
public:
    explicit SignedAvatarPackFixture(std::filesystem::path workspace);
    SignedAvatarPackFixture(
        std::filesystem::path workspace,
        const std::array<std::uint8_t, crypto_sign_SEEDBYTES>& seed);

    [[nodiscard]] TrustedAvatarKey trustedKey() const;
    [[nodiscard]] std::filesystem::path writePack(
        SignedPackOptions options = {});

private:
    std::filesystem::path workspace_;
    std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> publicKey_{};
    std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> secretKey_{};
};

}  // namespace creator::avatar_pack_adapter::test
