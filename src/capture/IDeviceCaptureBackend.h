#pragma once

#include "capture/DeviceCaptureTypes.h"
#include "capture/IAudioBlockSink.h"
#include "capture/IDeviceCaptureSource.h"
#include "capture/IVideoFrameSink.h"
#include "core/Result.h"

#include <functional>
#include <memory>
#include <vector>

namespace creator::capture {

/// Native device discovery, permission, hotplug, and source factory boundary.
class IDeviceCaptureBackend {
public:
    using PermissionCompletion =
        std::function<void(creator::core::Result<MediaPermissionStatus>)>;
    using DeviceChangeHandler = std::function<void()>;

    virtual ~IDeviceCaptureBackend() = default;
    IDeviceCaptureBackend(const IDeviceCaptureBackend&) = delete;
    IDeviceCaptureBackend& operator=(const IDeviceCaptureBackend&) = delete;
    IDeviceCaptureBackend(IDeviceCaptureBackend&&) = delete;
    IDeviceCaptureBackend& operator=(IDeviceCaptureBackend&&) = delete;

    [[nodiscard]] virtual MediaPermissionStatus permissionStatus(
        CaptureDeviceKind kind) const noexcept = 0;
    virtual void requestPermission(CaptureDeviceKind kind,
                                   PermissionCompletion completion) = 0;

    [[nodiscard]] virtual creator::core::Result<std::vector<CaptureDeviceInfo>> devices(
        CaptureDeviceKind kind) = 0;
    virtual void setDeviceChangeHandler(DeviceChangeHandler handler) = 0;

    [[nodiscard]] virtual creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
    createCamera(const creator::domain::CaptureDeviceId& deviceId,
                 std::shared_ptr<IVideoFrameSink> sink) = 0;
    [[nodiscard]] virtual creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
    createMicrophone(const creator::domain::CaptureDeviceId& deviceId,
                     std::shared_ptr<IAudioBlockSink> sink) = 0;
    [[nodiscard]] virtual creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
    createSystemAudio(std::shared_ptr<IAudioBlockSink> sink) = 0;

protected:
    IDeviceCaptureBackend() = default;
};

}  // namespace creator::capture
