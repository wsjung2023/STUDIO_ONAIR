#include "capture/DeviceCaptureTypes.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

CaptureDeviceInfo::CaptureDeviceInfo(creator::domain::CaptureDeviceId id,
                                     CaptureDeviceKind kind, std::string displayName,
                                     bool isDefault) noexcept
    : id_(std::move(id)),
      kind_(kind),
      displayName_(std::move(displayName)),
      isDefault_(isDefault) {}

creator::core::Result<CaptureDeviceInfo> CaptureDeviceInfo::create(
    creator::domain::CaptureDeviceId id, CaptureDeviceKind kind, std::string displayName,
    bool isDefault) {
    if (displayName.empty()) {
        return creator::core::AppError{creator::core::ErrorCode::InvalidArgument,
                                       "capture device display name must not be empty"};
    }
    return CaptureDeviceInfo{std::move(id), kind, std::move(displayName), isDefault};
}

}  // namespace creator::capture
