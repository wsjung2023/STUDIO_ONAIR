#include "platform_release/PlatformCapability.h"

#include "core/AppError.h"

#include <cctype>
#include <utility>

namespace creator::platform_release {
namespace {

bool isCapabilityId(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (const char character : value) {
        const unsigned char ascii = static_cast<unsigned char>(character);
        if (!(std::islower(ascii) || std::isdigit(ascii) || character == '-')) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view toString(CapabilityState state) noexcept {
    switch (state) {
    case CapabilityState::Available:
        return "available";
    case CapabilityState::PermissionRequired:
        return "permission-required";
    case CapabilityState::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

core::Result<PlatformCapability> PlatformCapability::create(
    std::string id, CapabilityState state, std::string unavailableReason) {
    if (!isCapabilityId(id)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "platform capability id must be lowercase letters, digits, or hyphens"};
    }
    if (state == CapabilityState::Available && !unavailableReason.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "an available platform capability must not have an unavailable reason"};
    }
    if (state != CapabilityState::Available && unavailableReason.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "an unavailable platform capability must explain how it is unavailable"};
    }
    return PlatformCapability{std::move(id), state, std::move(unavailableReason)};
}

}  // namespace creator::platform_release
