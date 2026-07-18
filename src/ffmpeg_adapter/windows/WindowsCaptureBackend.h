#pragma once

#include "capture/IDeviceCaptureBackend.h"
#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"

#include <memory>

namespace creator::ffmpeg_adapter::windows {

struct WindowsCaptureBackend final {
    std::unique_ptr<capture::IScreenCapturePermission> screenPermission;
    std::unique_ptr<capture::IScreenCaptureDiscovery> screenDiscovery;
    std::unique_ptr<capture::IScreenCaptureSourceFactory> screenSourceFactory;
    std::unique_ptr<capture::IDeviceCaptureBackend> devices;
};

/// Creates the real Windows capture adapters backed by the already-audited
/// FFmpeg DirectShow/GDI inputs and native WASAPI loopback.
[[nodiscard]] WindowsCaptureBackend makeWindowsCaptureBackend();

}  // namespace creator::ffmpeg_adapter::windows
