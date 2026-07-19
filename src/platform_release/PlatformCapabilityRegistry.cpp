#include "platform_release/PlatformCapabilityRegistry.h"

#include "core/AppError.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace creator::platform_release {
namespace {

PlatformCapability available(const char* id) {
    return PlatformCapability::create(id, CapabilityState::Available, "").value();
}

PlatformCapability permissionRequired(const char* id, const char* reason) {
    return PlatformCapability::create(id, CapabilityState::PermissionRequired, reason).value();
}

PlatformCapability unavailable(const char* id, const char* reason) {
    return PlatformCapability::create(id, CapabilityState::Unavailable, reason).value();
}

std::vector<PlatformCapability> productCapabilities() {
    return {
        available("project-recovery"),
        available("timeline-editing"),
        available("captions-ai"),
        available("audio-cleanup"),
        available("avatar"),
        available("export"),
    };
}

}  // namespace

std::string_view toString(PlatformKind platform) noexcept {
    switch (platform) {
    case PlatformKind::Windows:
        return "windows";
    case PlatformKind::MacOS:
        return "macos";
    case PlatformKind::Android:
        return "android";
    case PlatformKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

core::Result<PlatformCapabilityRegistry> PlatformCapabilityRegistry::create(
    PlatformKind platform, std::vector<PlatformCapability> capabilities) {
    std::unordered_set<std::string> identifiers;
    for (const auto& capability : capabilities) {
        if (!identifiers.insert(capability.id()).second) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "platform capability ids must be unique"};
        }
    }
    return PlatformCapabilityRegistry{platform, std::move(capabilities)};
}

PlatformCapabilityRegistry PlatformCapabilityRegistry::defaultsFor(PlatformKind platform) {
    auto capabilities = productCapabilities();
    switch (platform) {
    case PlatformKind::Windows:
        capabilities.push_back(permissionRequired(
            "screen-capture", "Windows screen capture permission is required."));
        capabilities.push_back(permissionRequired(
            "camera", "Camera permission is required."));
        capabilities.push_back(permissionRequired(
            "microphone", "Microphone permission is required."));
        capabilities.push_back(permissionRequired(
            "system-audio", "System audio access requires a supported Windows capture session."));
        capabilities.push_back(available("cursor-telemetry"));
        break;
    case PlatformKind::MacOS:
        capabilities.push_back(permissionRequired(
            "screen-capture", "Screen Recording permission is required in macOS settings."));
        capabilities.push_back(permissionRequired(
            "camera", "Camera permission is required in macOS settings."));
        capabilities.push_back(permissionRequired(
            "microphone", "Microphone permission is required in macOS settings."));
        capabilities.push_back(permissionRequired(
            "system-audio", "System audio requires an approved ScreenCaptureKit session."));
        capabilities.push_back(unavailable(
            "cursor-telemetry", "macOS global cursor telemetry is not implemented by this release."));
        break;
    case PlatformKind::Android:
        capabilities.push_back(permissionRequired(
            "screen-capture", "Android MediaProjection approval is required for screen capture."));
        capabilities.push_back(permissionRequired(
            "camera", "Android camera permission is required."));
        capabilities.push_back(permissionRequired(
            "microphone", "Android microphone permission is required."));
        capabilities.push_back(permissionRequired(
            "system-audio", "Android playback capture requires MediaProjection approval and app-policy support."));
        capabilities.push_back(unavailable(
            "cursor-telemetry", "Android does not provide unrestricted global pointer telemetry."));
        break;
    case PlatformKind::Unknown:
        capabilities.push_back(unavailable(
            "screen-capture", "This operating system is not a supported Creator Studio capture target."));
        capabilities.push_back(unavailable(
            "camera", "This operating system is not a supported Creator Studio camera target."));
        capabilities.push_back(unavailable(
            "microphone", "This operating system is not a supported Creator Studio microphone target."));
        capabilities.push_back(unavailable(
            "system-audio", "This operating system is not a supported Creator Studio audio target."));
        capabilities.push_back(unavailable(
            "cursor-telemetry", "This operating system is not a supported Creator Studio cursor target."));
        break;
    }
    return create(platform, std::move(capabilities)).value();
}

const PlatformCapability* PlatformCapabilityRegistry::find(std::string_view id) const noexcept {
    const auto found = std::find_if(
        capabilities_.begin(), capabilities_.end(),
        [id](const PlatformCapability& capability) { return capability.id() == id; });
    return found == capabilities_.end() ? nullptr : &*found;
}

PlatformKind currentPlatformKind() noexcept {
#if defined(_WIN32)
    return PlatformKind::Windows;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#elif defined(__ANDROID__)
    return PlatformKind::Android;
#else
    return PlatformKind::Unknown;
#endif
}

}  // namespace creator::platform_release
