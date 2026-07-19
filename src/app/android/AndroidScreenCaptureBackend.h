#pragma once

#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"

#include <memory>

namespace creator::app::android {

/// Android implementation of the existing screen-capture port. Java owns the
/// platform consent activity; this factory exposes its result to Qt without
/// allowing Android types below the application boundary.
struct AndroidScreenCaptureBackend final {
    std::unique_ptr<capture::IScreenCapturePermission> permission;
    std::unique_ptr<capture::IScreenCaptureDiscovery> discovery;
    std::unique_ptr<capture::IScreenCaptureSourceFactory> sourceFactory;
};

[[nodiscard]] AndroidScreenCaptureBackend makeAndroidScreenCaptureBackend();

}  // namespace creator::app::android
