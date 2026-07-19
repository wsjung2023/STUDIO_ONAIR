#include "platform_release/MobilePerformancePolicy.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

namespace {

using creator::core::ErrorCode;
using creator::platform_release::DevicePerformanceClass;
using creator::platform_release::MobileDeviceProfile;
using creator::platform_release::MobilePerformancePolicy;
using creator::platform_release::ThermalState;

TEST(MobilePerformancePolicyTest, RejectsUnknownMemoryClass) {
    const auto policy = MobilePerformancePolicy::create(
        MobileDeviceProfile{.memoryMiB = 0});

    ASSERT_FALSE(policy.hasValue());
    EXPECT_EQ(policy.error().code(), ErrorCode::InvalidArgument);
}

TEST(MobilePerformancePolicyTest, LowRamDeviceUsesDeterministicSafeBudget) {
    const auto policy = MobilePerformancePolicy::create(
        MobileDeviceProfile{.memoryMiB = 8'192, .lowRamDevice = true});

    ASSERT_TRUE(policy.hasValue());
    const auto& budget = policy.value().budget();
    EXPECT_EQ(policy.value().deviceClass(), DevicePerformanceClass::Constrained);
    EXPECT_EQ(budget.previewWidth, 640U);
    EXPECT_EQ(budget.previewHeight, 360U);
    EXPECT_EQ(budget.previewFramesPerSecond, 24U);
    EXPECT_EQ(budget.concurrentVideoDecoders, 1U);
    EXPECT_EQ(budget.maximumExportHeight, 1'080U);
    EXPECT_EQ(budget.maximumModelBytes, 512ULL * 1024ULL * 1024ULL);
    EXPECT_TRUE(budget.foregroundExportRequired);
}

TEST(MobilePerformancePolicyTest, PowerSaveModeDemotesHighMemoryDevice) {
    const auto policy = MobilePerformancePolicy::create(
        MobileDeviceProfile{.memoryMiB = 12'288, .powerSaveMode = true});

    ASSERT_TRUE(policy.hasValue());
    EXPECT_EQ(policy.value().deviceClass(), DevicePerformanceClass::Balanced);
    EXPECT_EQ(policy.value().budget().previewHeight, 540U);
    EXPECT_EQ(policy.value().budget().maximumExportHeight, 1'080U);
}

TEST(MobilePerformancePolicyTest, PerformanceDeviceAllowsBounded4kExport) {
    const auto policy = MobilePerformancePolicy::create(
        MobileDeviceProfile{.memoryMiB = 8'192});

    ASSERT_TRUE(policy.hasValue());
    EXPECT_EQ(policy.value().deviceClass(), DevicePerformanceClass::Performance);
    EXPECT_EQ(policy.value().budget().previewWidth, 1'280U);
    EXPECT_EQ(policy.value().budget().maximumExportHeight, 2'160U);
    EXPECT_EQ(policy.value().budget().maximumModelBytes,
              4ULL * 1024ULL * 1024ULL * 1024ULL);
}

TEST(MobilePerformancePolicyTest, SeriousThermalStateReducesPreviewAndExport) {
    const auto policy = MobilePerformancePolicy::create(
        MobileDeviceProfile{.memoryMiB = 8'192,
                            .thermalState = ThermalState::Serious});

    ASSERT_TRUE(policy.hasValue());
    EXPECT_EQ(policy.value().budget().previewWidth, 640U);
    EXPECT_EQ(policy.value().budget().previewFramesPerSecond, 15U);
    EXPECT_EQ(policy.value().budget().maximumExportHeight, 1'080U);
    EXPECT_TRUE(policy.value().budget().exportAllowed);
}

TEST(MobilePerformancePolicyTest, CriticalThermalStateBlocksNewExport) {
    const auto policy = MobilePerformancePolicy::create(
        MobileDeviceProfile{.memoryMiB = 8'192,
                            .thermalState = ThermalState::Critical});

    ASSERT_TRUE(policy.hasValue());
    EXPECT_FALSE(policy.value().budget().exportAllowed);
    EXPECT_EQ(policy.value().budget().previewWidth, 480U);
    EXPECT_EQ(policy.value().budget().previewFramesPerSecond, 12U);
}

}  // namespace
