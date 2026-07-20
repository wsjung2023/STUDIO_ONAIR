#pragma once

#include "capture/IDeviceCaptureBackend.h"

#include <memory>

namespace creator::app::android {

/// Android Camera2/AudioRecord adapter. Platform types remain in this app boundary.
[[nodiscard]] std::unique_ptr<capture::IDeviceCaptureBackend>
makeAndroidDeviceCaptureBackend();

}  // namespace creator::app::android
