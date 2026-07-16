#pragma once

#include "capture/IDeviceCaptureBackend.h"

namespace creator::capture {

/// Explicit fallback for platforms without an R0-04 native adapter.
class UnsupportedDeviceCaptureBackend final : public IDeviceCaptureBackend {
public:
    [[nodiscard]] MediaPermissionStatus permissionStatus(
        CaptureDeviceKind kind) const noexcept override;
    void requestPermission(CaptureDeviceKind kind,
                           PermissionCompletion completion) override;
    [[nodiscard]] creator::core::Result<std::vector<CaptureDeviceInfo>> devices(
        CaptureDeviceKind kind) override;
    void setDeviceChangeHandler(DeviceChangeHandler handler) override;

    [[nodiscard]] creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
    createCamera(const creator::domain::CaptureDeviceId& deviceId,
                 std::shared_ptr<IVideoFrameSink> sink) override;
    [[nodiscard]] creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
    createMicrophone(const creator::domain::CaptureDeviceId& deviceId,
                     std::shared_ptr<IAudioBlockSink> sink) override;
    [[nodiscard]] creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
    createSystemAudio(std::shared_ptr<IAudioBlockSink> sink) override;
};

}  // namespace creator::capture
