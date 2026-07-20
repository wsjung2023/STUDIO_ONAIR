#pragma once

#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"

#include <memory>

namespace creator::capture::windows {

/// Bundles the three screen-capture ports backed by Windows.Graphics.Capture
/// (WGC) + Direct3D11. Mirrors the Apple-only MacScreenCaptureBackend so the
/// application wires either platform through the identical port trio.
///
/// This symbol exists only in the Windows-only cs_capture_windows target.
struct WindowsScreenCaptureBackend final {
    std::unique_ptr<IScreenCapturePermission> permission;
    std::unique_ptr<IScreenCaptureDiscovery> discovery;
    std::unique_ptr<IScreenCaptureSourceFactory> sourceFactory;
};

/// Builds three ports that share one monitor-discovery snapshot. The WGC session
/// (Windows 10 1903+) needs no user-facing permission prompt, so permission is a
/// capability probe rather than an OS grant flow.
[[nodiscard]] WindowsScreenCaptureBackend makeWindowsScreenCaptureBackend();

}  // namespace creator::capture::windows
