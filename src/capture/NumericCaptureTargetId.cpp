#include "capture/NumericCaptureTargetId.h"

#include <string>

namespace creator::capture {

core::Result<domain::CaptureTargetId> makeNumericCaptureTargetId(
    ScreenCaptureTargetKind kind, std::uint64_t nativeId) {
    const char* prefix = kind == ScreenCaptureTargetKind::Display ? "display:" : "window:";
    return domain::CaptureTargetId::create(std::string{prefix} + std::to_string(nativeId));
}

}  // namespace creator::capture

