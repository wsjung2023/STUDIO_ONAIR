#include "avatar_pack_adapter/SodiumSignatureVerifier.h"

#include "core/AppError.h"

#include <cstdint>

namespace creator::avatar_pack_adapter {
namespace {

core::AppError signatureError(core::ErrorCode code, std::string message,
                              std::string issueCode) {
    return {code, std::move(message), std::move(issueCode),
            "avatar.validation.signature"};
}

}  // namespace

core::Result<void> SodiumSignatureVerifier::initialize() const {
    static const int initialized = sodium_init();
    if (initialized < 0) {
        return signatureError(core::ErrorCode::InvalidState,
                              "avatar pack cryptography initialization failed",
                              "avatar.pack.signature.initialize");
    }
    return core::ok();
}

core::Result<void> SodiumSignatureVerifier::validatePublicKey(
    const std::array<std::byte, crypto_sign_PUBLICKEYBYTES>& publicKey)
    const {
    if (auto initialized = initialize(); !initialized.hasValue())
        return initialized.error();
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(publicKey.data());
    if (crypto_core_ed25519_is_valid_point(bytes) != 1) {
        return signatureError(core::ErrorCode::InvalidArgument,
                              "avatar pack trusted public key is invalid",
                              "avatar.pack.trusted-key.bytes");
    }
    return core::ok();
}

core::Result<void> SodiumSignatureVerifier::verifyDetached(
    const std::array<std::byte, crypto_sign_BYTES>& signature,
    std::span<const std::uint8_t> message,
    const std::array<std::byte, crypto_sign_PUBLICKEYBYTES>& publicKey)
    const {
    if (auto valid = validatePublicKey(publicKey); !valid.hasValue())
        return valid.error();
    const auto verified = crypto_sign_verify_detached(
        reinterpret_cast<const unsigned char*>(signature.data()),
        message.data(), static_cast<unsigned long long>(message.size()),
        reinterpret_cast<const unsigned char*>(publicKey.data()));
    if (verified != 0) {
        return signatureError(core::ErrorCode::InvalidArgument,
                              "avatar pack signature is invalid",
                              "avatar.pack.signature.verify");
    }
    return core::ok();
}

}  // namespace creator::avatar_pack_adapter
