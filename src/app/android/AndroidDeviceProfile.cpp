#include "app/android/AndroidDeviceProfile.h"

#include "core/AppError.h"

#include <QJniEnvironment>
#include <QJniObject>

namespace creator::app::android {
namespace {

constexpr const char* kActivityClass =
    "com/studioonair/creatorstudio/CreatorStudioActivity";

platform_release::ThermalState thermalState(jint value) noexcept {
    if (value >= 2) return platform_release::ThermalState::Critical;
    if (value == 1) return platform_release::ThermalState::Serious;
    return platform_release::ThermalState::Nominal;
}

}  // namespace

core::Result<platform_release::MobilePerformancePolicy>
currentAndroidPerformancePolicy() {
    const auto memoryMiB = QJniObject::callStaticMethod<jint>(
        kActivityClass, "deviceMemoryClassMiB", "()I");
    const auto lowRam = QJniObject::callStaticMethod<jboolean>(
        kActivityClass, "isLowRamDevice", "()Z");
    const auto powerSave = QJniObject::callStaticMethod<jboolean>(
        kActivityClass, "isPowerSaveMode", "()Z");
    const auto thermal = QJniObject::callStaticMethod<jint>(
        kActivityClass, "thermalState", "()I");
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions()) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Android device resource profile is unavailable"};
    }
    return platform_release::MobilePerformancePolicy::create({
        .memoryMiB = memoryMiB > 0 ? static_cast<std::uint32_t>(memoryMiB)
                                  : 2'048U,
        .lowRamDevice = lowRam == JNI_TRUE || memoryMiB <= 0,
        .powerSaveMode = powerSave == JNI_TRUE,
        .thermalState = thermalState(thermal),
    });
}

}  // namespace creator::app::android
