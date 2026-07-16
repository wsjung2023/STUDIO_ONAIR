#include "recorder/DiskSpaceMonitor.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>

namespace {

using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::recorder::DiskSpaceMonitor;
using creator::recorder::DiskSpaceValues;
using creator::recorder::IDiskSpaceProbe;

class SpaceProbeFake final : public IDiskSpaceProbe {
public:
    Result<DiskSpaceValues> query(const std::filesystem::path& path) override {
        ++calls;
        lastPath = path;
        if (error) return *error;
        return values;
    }

    DiskSpaceValues values{};
    std::optional<AppError> error;
    std::filesystem::path lastPath;
    int calls{0};
};

TEST(DiskSpaceMonitorTest, AcceptsExactReservePlusNextSegmentEstimate) {
    auto probe = std::make_unique<SpaceProbeFake>();
    auto* raw = probe.get();
    raw->values = {.capacityBytes = 10'000, .freeBytes = 8'000, .availableBytes = 1'500};
    DiskSpaceMonitor monitor{std::move(probe), 1'000};

    const auto result = monitor.check("project.cstudio", 500);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().requiredBytes, 1'500u);
    EXPECT_EQ(result.value().availableBytes, 1'500u);
    EXPECT_TRUE(result.value().sufficient);
    EXPECT_EQ(raw->lastPath, std::filesystem::path{"project.cstudio"});
}

TEST(DiskSpaceMonitorTest, RejectsOneByteBelowRequiredAndRetainsDiagnostics) {
    auto probe = std::make_unique<SpaceProbeFake>();
    probe->values.availableBytes = 1'499;
    DiskSpaceMonitor monitor{std::move(probe), 1'000};

    const auto result = monitor.check("project.cstudio", 500);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InsufficientStorage);
    ASSERT_TRUE(monitor.lastSnapshot().has_value());
    EXPECT_EQ(monitor.lastSnapshot()->availableBytes, 1'499u);
    EXPECT_FALSE(monitor.lastSnapshot()->sufficient);
}

TEST(DiskSpaceMonitorTest, RejectsRequiredByteOverflowWithoutQueryingFilesystem) {
    auto probe = std::make_unique<SpaceProbeFake>();
    auto* raw = probe.get();
    DiskSpaceMonitor monitor{std::move(probe), std::numeric_limits<std::uint64_t>::max()};

    const auto result = monitor.check("project.cstudio", 1);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(raw->calls, 0);
}

TEST(DiskSpaceMonitorTest, PropagatesFilesystemProbeErrorExactly) {
    auto probe = std::make_unique<SpaceProbeFake>();
    probe->error = AppError{ErrorCode::IoFailure, "space probe failed"};
    DiskSpaceMonitor monitor{std::move(probe), 1'000};

    const auto result = monitor.check("project.cstudio", 500);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), *monitor.lastError());
    EXPECT_EQ(result.error().message(), "space probe failed");
}

}  // namespace
