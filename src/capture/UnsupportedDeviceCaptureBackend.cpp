#include "capture/UnsupportedDeviceCaptureBackend.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {
namespace {

creator::core::AppError unsupported() {
    return {creator::core::ErrorCode::UnsupportedVersion,
            "Camera and audio capture are available on the macOS build"};
}

}  // namespace

MediaPermissionStatus UnsupportedDeviceCaptureBackend::permissionStatus(
    CaptureDeviceKind) const noexcept {
    return MediaPermissionStatus::Denied;
}

void UnsupportedDeviceCaptureBackend::requestPermission(
    CaptureDeviceKind, PermissionCompletion completion) {
    completion(unsupported());
}

creator::core::Result<std::vector<CaptureDeviceInfo>>
UnsupportedDeviceCaptureBackend::devices(CaptureDeviceKind) {
    return unsupported();
}

void UnsupportedDeviceCaptureBackend::setDeviceChangeHandler(DeviceChangeHandler) {}

creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
UnsupportedDeviceCaptureBackend::createCamera(const creator::domain::CaptureDeviceId&,
                                              std::shared_ptr<IVideoFrameSink>) {
    return unsupported();
}

creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
UnsupportedDeviceCaptureBackend::createMicrophone(const creator::domain::CaptureDeviceId&,
                                                  std::shared_ptr<IAudioBlockSink>) {
    return unsupported();
}

creator::core::Result<std::unique_ptr<IDeviceCaptureSource>>
UnsupportedDeviceCaptureBackend::createSystemAudio(std::shared_ptr<IAudioBlockSink>) {
    return unsupported();
}

}  // namespace creator::capture
