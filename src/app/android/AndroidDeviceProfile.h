#pragma once

#include "core/Result.h"
#include "platform_release/MobilePerformancePolicy.h"

namespace creator::app::android {

[[nodiscard]] core::Result<platform_release::MobilePerformancePolicy>
currentAndroidPerformancePolicy();

}  // namespace creator::app::android
