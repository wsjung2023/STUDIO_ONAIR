#pragma once

#include "core/Result.h"

#include <string>
#include <string_view>

namespace creator::platform_release {

/// The externally visible state of one product capability on a platform.
///
/// PermissionRequired is deliberately distinct from Unavailable: the former
/// can be enabled by an informed user action, while the latter must not be
/// presented as an action the application can complete.
enum class CapabilityState {
    Available,
    PermissionRequired,
    Unavailable,
};

[[nodiscard]] std::string_view toString(CapabilityState state) noexcept;

/// A platform-neutral capability snapshot consumed by the application layer.
/// The id is a stable lowercase token (for example "screen-capture"), never an
/// OS API name. Platform-specific adapters map their runtime result into this
/// value so QML and shared workflows do not infer support from an OS name.
class PlatformCapability final {
public:
    [[nodiscard]] static core::Result<PlatformCapability> create(
        std::string id, CapabilityState state, std::string unavailableReason);

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] CapabilityState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& unavailableReason() const noexcept {
        return unavailableReason_;
    }

    friend bool operator==(const PlatformCapability&, const PlatformCapability&) = default;

private:
    PlatformCapability(std::string id, CapabilityState state,
                       std::string unavailableReason)
        : id_(std::move(id)),
          state_(state),
          unavailableReason_(std::move(unavailableReason)) {}

    std::string id_;
    CapabilityState state_;
    std::string unavailableReason_;
};

}  // namespace creator::platform_release
