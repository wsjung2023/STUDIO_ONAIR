#include "capture/UnsupportedScreenCaptureBackend.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {
namespace {

core::AppError unavailable() {
    return core::AppError{core::ErrorCode::InvalidState,
                          "Native screen capture is unavailable on this platform"};
}

}  // namespace

ScreenCapturePermissionStatus UnsupportedScreenCapturePermission::status() const noexcept {
    return ScreenCapturePermissionStatus::Denied;
}

void UnsupportedScreenCapturePermission::request(Completion completion) {
    completion(unavailable());
}

void UnsupportedScreenCaptureDiscovery::enumerate(Completion completion) {
    completion(unavailable());
}

core::Result<std::unique_ptr<IScreenCaptureSource>> UnsupportedScreenCaptureSourceFactory::create(
    const domain::CaptureTargetId&, std::shared_ptr<IVideoFrameSink>) {
    return unavailable();
}

}  // namespace creator::capture
