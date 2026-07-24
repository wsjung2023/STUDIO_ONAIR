#pragma once

#include "core/Result.h"

#include <sodium.h>

#include <array>
#include <cstddef>
#include <span>

namespace creator::avatar_pack_adapter {

/// Validates Ed25519 public keys and detached signatures through libsodium.
class SodiumSignatureVerifier final {
public:
    [[nodiscard]] core::Result<void> validatePublicKey(
        const std::array<std::byte, crypto_sign_PUBLICKEYBYTES>& publicKey)
        const;
    [[nodiscard]] core::Result<void> verifyDetached(
        const std::array<std::byte, crypto_sign_BYTES>& signature,
        std::span<const std::uint8_t> message,
        const std::array<std::byte, crypto_sign_PUBLICKEYBYTES>& publicKey)
        const;

private:
    [[nodiscard]] core::Result<void> initialize() const;
};

}  // namespace creator::avatar_pack_adapter
