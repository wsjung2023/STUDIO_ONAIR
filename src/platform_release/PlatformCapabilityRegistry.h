#pragma once

#include "core/Result.h"
#include "platform_release/PlatformCapability.h"

#include <span>
#include <string_view>
#include <vector>

namespace creator::platform_release {

enum class PlatformKind {
    Windows,
    MacOS,
    Android,
    Unknown,
};

[[nodiscard]] std::string_view toString(PlatformKind platform) noexcept;

/// Immutable registry for one platform's capability contract. Runtime adapters
/// may construct a new registry after a permission or encoder probe changes;
/// callers never mutate an already-published snapshot.
class PlatformCapabilityRegistry final {
public:
    [[nodiscard]] static core::Result<PlatformCapabilityRegistry> create(
        PlatformKind platform, std::vector<PlatformCapability> capabilities);

    /// Baseline product contract before device-specific permission and encoder
    /// probes. Android is intentionally listed as a complete editing target,
    /// not a companion device; policy-limited inputs remain explicit.
    [[nodiscard]] static PlatformCapabilityRegistry defaultsFor(PlatformKind platform);

    [[nodiscard]] PlatformKind platform() const noexcept { return platform_; }
    [[nodiscard]] std::span<const PlatformCapability> capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] const PlatformCapability* find(std::string_view id) const noexcept;

private:
    PlatformCapabilityRegistry(PlatformKind platform,
                               std::vector<PlatformCapability> capabilities)
        : platform_(platform), capabilities_(std::move(capabilities)) {}

    PlatformKind platform_;
    std::vector<PlatformCapability> capabilities_;
};

[[nodiscard]] PlatformKind currentPlatformKind() noexcept;

}  // namespace creator::platform_release
