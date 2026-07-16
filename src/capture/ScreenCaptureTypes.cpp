#include "capture/ScreenCaptureTypes.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

core::Result<ScreenCaptureTarget> ScreenCaptureTarget::create(
    domain::CaptureTargetId id, ScreenCaptureTargetKind kind, std::string displayName,
    std::optional<std::string> applicationName, std::uint32_t width,
    std::uint32_t height) {
    if (displayName.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "screen capture target name must not be empty"};
    }
    if (width == 0 || height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "screen capture target dimensions must be positive"};
    }
    if (applicationName && applicationName->empty()) {
        applicationName.reset();
    }
    return ScreenCaptureTarget{std::move(id), kind, std::move(displayName),
                               std::move(applicationName), width, height};
}

ScreenCaptureTarget::ScreenCaptureTarget(domain::CaptureTargetId id,
                                         ScreenCaptureTargetKind kind,
                                         std::string displayName,
                                         std::optional<std::string> applicationName,
                                         std::uint32_t width,
                                         std::uint32_t height) noexcept
    : id_(std::move(id)),
      kind_(kind),
      displayName_(std::move(displayName)),
      applicationName_(std::move(applicationName)),
      width_(width),
      height_(height) {}

}  // namespace creator::capture

