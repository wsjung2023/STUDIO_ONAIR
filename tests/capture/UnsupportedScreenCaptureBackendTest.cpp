#include "capture/UnsupportedScreenCaptureBackend.h"

#include "capture/LatestVideoFrameMailbox.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using creator::capture::ScreenCapturePermissionStatus;
using creator::capture::UnsupportedScreenCaptureDiscovery;
using creator::capture::UnsupportedScreenCapturePermission;
using creator::capture::UnsupportedScreenCaptureSourceFactory;
using creator::core::ErrorCode;

TEST(UnsupportedScreenCaptureBackendTest, PermissionRequestExplainsPlatformBoundary) {
    UnsupportedScreenCapturePermission permission;
    std::optional<creator::core::Result<ScreenCapturePermissionStatus>> result;

    EXPECT_EQ(permission.status(), ScreenCapturePermissionStatus::Denied);
    permission.request([&result](auto value) { result.emplace(std::move(value)); });

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->hasValue());
    EXPECT_EQ(result->error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(result->error().message(),
              "Native screen capture is unavailable on this platform");
}

TEST(UnsupportedScreenCaptureBackendTest, DiscoveryAndFactoryFailWithoutInventedTargets) {
    UnsupportedScreenCaptureDiscovery discovery;
    std::optional<creator::core::Result<std::vector<creator::capture::ScreenCaptureTarget>>>
        discovered;
    discovery.enumerate(
        [&discovered](auto value) { discovered.emplace(std::move(value)); });
    ASSERT_TRUE(discovered.has_value());
    ASSERT_FALSE(discovered->hasValue());

    UnsupportedScreenCaptureSourceFactory factory;
    const auto created = factory.create(
        creator::domain::CaptureTargetId::create("unsupported:1").value(),
        std::make_shared<creator::capture::LatestVideoFrameMailbox>());
    ASSERT_FALSE(created.hasValue());
    EXPECT_EQ(created.error().code(), ErrorCode::InvalidState);
}

}  // namespace

