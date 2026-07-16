#pragma once

#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"

#include <memory>

namespace creator::capture::macos {

struct MacScreenCaptureBackend final {
    std::unique_ptr<IScreenCapturePermission> permission;
    std::unique_ptr<IScreenCaptureDiscovery> discovery;
    std::unique_ptr<IScreenCaptureSourceFactory> sourceFactory;
};

/// Builds three ports that share one retained ScreenCaptureKit target snapshot.
/// This symbol exists only in the Apple-only cs_capture_macos target.
[[nodiscard]] MacScreenCaptureBackend makeMacScreenCaptureBackend();

}  // namespace creator::capture::macos

