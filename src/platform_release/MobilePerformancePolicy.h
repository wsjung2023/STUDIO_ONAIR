#pragma once

#include "core/Result.h"

#include <cstdint>

namespace creator::platform_release {

enum class DevicePerformanceClass {
    Constrained,
    Balanced,
    Performance,
};

enum class ThermalState {
    Nominal,
    Serious,
    Critical,
};

struct MobileDeviceProfile final {
    std::uint32_t memoryMiB{};
    bool lowRamDevice{};
    bool powerSaveMode{};
    ThermalState thermalState{ThermalState::Nominal};
};

struct MobilePerformanceBudget final {
    std::uint32_t previewWidth{};
    std::uint32_t previewHeight{};
    std::uint32_t previewFramesPerSecond{};
    std::uint32_t concurrentVideoDecoders{};
    std::uint32_t maximumExportHeight{};
    std::uint64_t maximumModelBytes{};
    bool foregroundExportRequired{true};
    bool exportAllowed{true};

    friend bool operator==(const MobilePerformanceBudget&,
                           const MobilePerformanceBudget&) = default;
};

/// Maps observable Android resource signals to deterministic product limits.
/// It contains no platform API calls, so the same inputs produce the same
/// fallback on every build and can be tested without a device.
class MobilePerformancePolicy final {
public:
    [[nodiscard]] static core::Result<MobilePerformancePolicy> create(
        MobileDeviceProfile profile);

    [[nodiscard]] DevicePerformanceClass deviceClass() const noexcept {
        return deviceClass_;
    }
    [[nodiscard]] const MobilePerformanceBudget& budget() const noexcept {
        return budget_;
    }

private:
    MobilePerformancePolicy(DevicePerformanceClass deviceClass,
                            MobilePerformanceBudget budget)
        : deviceClass_(deviceClass), budget_(budget) {}

    DevicePerformanceClass deviceClass_;
    MobilePerformanceBudget budget_;
};

}  // namespace creator::platform_release
