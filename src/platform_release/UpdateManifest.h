#pragma once

#include "core/Result.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace creator::platform_release {

struct UpdateTarget final {
    std::string platform;
    std::string artifact;
    std::string url;
    std::string sha256;
    std::uint64_t sizeBytes{};

    friend bool operator==(const UpdateTarget&, const UpdateTarget&) = default;
};

class IUpdateSignatureVerifier {
public:
    virtual ~IUpdateSignatureVerifier() = default;
    [[nodiscard]] virtual core::Result<void> verify(
        std::string_view canonicalPayload,
        std::span<const std::byte> signature) const = 0;
};

/// Strict, immutable metadata covered by a detached signature. Network I/O and
/// key selection intentionally live outside this value type.
class UpdateManifest final {
public:
    [[nodiscard]] static core::Result<UpdateManifest> create(
        std::string channel, std::string productVersion,
        std::vector<UpdateTarget> targets);
    [[nodiscard]] static core::Result<UpdateManifest> fromPayloadJson(
        const nlohmann::json& document);

    [[nodiscard]] nlohmann::json toPayloadJson() const;
    [[nodiscard]] std::string canonicalPayload() const;
    [[nodiscard]] const std::string& channel() const noexcept { return channel_; }
    [[nodiscard]] const std::string& productVersion() const noexcept {
        return productVersion_;
    }
    [[nodiscard]] const std::vector<UpdateTarget>& targets() const noexcept {
        return targets_;
    }

    friend bool operator==(const UpdateManifest&, const UpdateManifest&) = default;

private:
    UpdateManifest(std::string channel, std::string productVersion,
                   std::vector<UpdateTarget> targets)
        : channel_(std::move(channel)), productVersion_(std::move(productVersion)),
          targets_(std::move(targets)) {}

    std::string channel_;
    std::string productVersion_;
    std::vector<UpdateTarget> targets_;
};

}  // namespace creator::platform_release
