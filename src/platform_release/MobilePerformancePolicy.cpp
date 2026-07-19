#include "platform_release/MobilePerformancePolicy.h"

#include "core/AppError.h"

#include <algorithm>

namespace creator::platform_release {
namespace {

constexpr std::uint64_t mebibytes(std::uint64_t value) noexcept {
    return value * 1024ULL * 1024ULL;
}

MobilePerformanceBudget constrainedBudget() {
    return {
        .previewWidth = 640,
        .previewHeight = 360,
        .previewFramesPerSecond = 24,
        .concurrentVideoDecoders = 1,
        .maximumExportHeight = 1'080,
        .maximumModelBytes = mebibytes(512),
        .foregroundExportRequired = true,
        .exportAllowed = true,
    };
}

MobilePerformanceBudget balancedBudget() {
    return {
        .previewWidth = 960,
        .previewHeight = 540,
        .previewFramesPerSecond = 30,
        .concurrentVideoDecoders = 2,
        .maximumExportHeight = 1'080,
        .maximumModelBytes = mebibytes(1'536),
        .foregroundExportRequired = true,
        .exportAllowed = true,
    };
}

MobilePerformanceBudget performanceBudget() {
    return {
        .previewWidth = 1'280,
        .previewHeight = 720,
        .previewFramesPerSecond = 30,
        .concurrentVideoDecoders = 3,
        .maximumExportHeight = 2'160,
        .maximumModelBytes = mebibytes(4'096),
        .foregroundExportRequired = true,
        .exportAllowed = true,
    };
}

void applyThermalLimit(ThermalState state, MobilePerformanceBudget& budget) {
    if (state == ThermalState::Nominal) return;
    if (state == ThermalState::Serious) {
        budget.previewWidth = std::min(budget.previewWidth, 640U);
        budget.previewHeight = std::min(budget.previewHeight, 360U);
        budget.previewFramesPerSecond = std::min(
            budget.previewFramesPerSecond, 15U);
        budget.concurrentVideoDecoders = 1;
        budget.maximumExportHeight = std::min(
            budget.maximumExportHeight, 1'080U);
        return;
    }
    budget.previewWidth = 480;
    budget.previewHeight = 270;
    budget.previewFramesPerSecond = 12;
    budget.concurrentVideoDecoders = 1;
    budget.exportAllowed = false;
}

}  // namespace

core::Result<MobilePerformancePolicy> MobilePerformancePolicy::create(
    MobileDeviceProfile profile) {
    if (profile.memoryMiB == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "mobile memory class must be known"};
    }

    DevicePerformanceClass deviceClass;
    MobilePerformanceBudget budget;
    if (profile.lowRamDevice || profile.memoryMiB < 3'072) {
        deviceClass = DevicePerformanceClass::Constrained;
        budget = constrainedBudget();
    } else if (profile.powerSaveMode || profile.memoryMiB < 6'144) {
        deviceClass = DevicePerformanceClass::Balanced;
        budget = balancedBudget();
    } else {
        deviceClass = DevicePerformanceClass::Performance;
        budget = performanceBudget();
    }
    applyThermalLimit(profile.thermalState, budget);
    return MobilePerformancePolicy{deviceClass, budget};
}

}  // namespace creator::platform_release
