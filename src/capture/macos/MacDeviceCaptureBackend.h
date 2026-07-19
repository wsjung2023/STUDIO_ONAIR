#pragma once

#include "capture/IDeviceCaptureBackend.h"

#include <memory>

namespace creator::capture::macos {

/// Creates the AVFoundation/ScreenCaptureKit camera and audio backend.
[[nodiscard]] std::unique_ptr<IDeviceCaptureBackend> makeMacDeviceCaptureBackend();

}  // namespace creator::capture::macos
