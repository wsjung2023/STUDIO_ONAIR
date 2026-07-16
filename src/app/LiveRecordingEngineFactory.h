#pragma once

#include "app/ILiveRecordingEngine.h"

#include <memory>

namespace creator::project_store {
class IProjectPackageStore;
}

namespace creator::app {

class DeviceCaptureController;
class ScreenCaptureController;

[[nodiscard]] std::unique_ptr<ILiveRecordingEngine> makeLiveRecordingEngine(
    ScreenCaptureController* screen, DeviceCaptureController* devices,
    std::shared_ptr<project_store::IProjectPackageStore> store);

}  // namespace creator::app
