#pragma once

#include "capture/ScreenCaptureTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <cstdint>

namespace creator::capture {

/// Converts a numeric OS display/window id into an opaque, kind-namespaced id.
/// The value is session-scoped and deliberately carries no persistence claim.
[[nodiscard]] creator::core::Result<creator::domain::CaptureTargetId>
makeNumericCaptureTargetId(ScreenCaptureTargetKind kind, std::uint64_t nativeId);

}  // namespace creator::capture

