#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/ScreenCaptureTypes.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using creator::capture::IScreenCaptureDiscovery;
using creator::capture::IScreenCapturePermission;
using creator::capture::ScreenCapturePermissionStatus;
using creator::capture::ScreenCaptureTarget;
using creator::capture::ScreenCaptureTargetKind;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::domain::CaptureTargetId;

CaptureTargetId targetId(std::string value = "display:42") {
    return CaptureTargetId::create(std::move(value)).value();
}

TEST(ScreenCaptureTargetTest, CreatesDisplayWithSessionScopedTypedIdentity) {
    const auto result = ScreenCaptureTarget::create(targetId(), ScreenCaptureTargetKind::Display,
                                                    "내장 디스플레이", std::nullopt, 2560,
                                                    1600);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().id().value(), "display:42");
    EXPECT_EQ(result.value().kind(), ScreenCaptureTargetKind::Display);
    EXPECT_EQ(result.value().displayName(), "내장 디스플레이");
    EXPECT_FALSE(result.value().applicationName().has_value());
    EXPECT_EQ(result.value().width(), 2560u);
    EXPECT_EQ(result.value().height(), 1600u);
}

TEST(ScreenCaptureTargetTest, CreatesWindowWithOwningApplication) {
    const auto result = ScreenCaptureTarget::create(
        targetId("window:7"), ScreenCaptureTargetKind::Window, "강의 슬라이드",
        std::optional<std::string>{"Keynote"}, 1920, 1080);

    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().applicationName().has_value());
    EXPECT_EQ(*result.value().applicationName(), "Keynote");
}

TEST(ScreenCaptureTargetTest, RejectsMissingDisplayNameOrGeometry) {
    for (const auto& fixture : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
             {0, 1080}, {1920, 0}}) {
        const auto result = ScreenCaptureTarget::create(
            targetId(), ScreenCaptureTargetKind::Display, "Display", std::nullopt,
            fixture.first, fixture.second);
        ASSERT_FALSE(result.hasValue());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }

    const auto unnamed = ScreenCaptureTarget::create(
        targetId(), ScreenCaptureTargetKind::Display, "", std::nullopt, 1920, 1080);
    ASSERT_FALSE(unnamed.hasValue());
    EXPECT_EQ(unnamed.error().code(), ErrorCode::InvalidArgument);
}

static_assert(!std::is_same_v<creator::domain::CaptureTargetId,
                              creator::domain::SourceId>);

class DiscoveryStub final : public IScreenCaptureDiscovery {
public:
    void enumerate(Completion completion) override {
        ++calls;
        std::vector<ScreenCaptureTarget> targets;
        targets.push_back(ScreenCaptureTarget::create(
                              targetId(), ScreenCaptureTargetKind::Display, "Display",
                              std::nullopt, 1920, 1080)
                              .value());
        completion(std::move(targets));
    }

    int calls{0};
};

class PermissionStub final : public IScreenCapturePermission {
public:
    [[nodiscard]] ScreenCapturePermissionStatus status() const noexcept override {
        return current;
    }

    void request(Completion completion) override {
        ++requests;
        current = ScreenCapturePermissionStatus::Granted;
        completion(current);
    }

    ScreenCapturePermissionStatus current{ScreenCapturePermissionStatus::Unknown};
    int requests{0};
};

TEST(ScreenCapturePortsTest, DiscoveryReturnsOneImmutableTargetListPerCompletion) {
    DiscoveryStub discovery;
    std::optional<Result<std::vector<ScreenCaptureTarget>>> result;

    discovery.enumerate([&result](auto value) { result.emplace(std::move(value)); });

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->hasValue());
    ASSERT_EQ(result->value().size(), 1u);
    EXPECT_EQ(result->value().front().id().value(), "display:42");
    EXPECT_EQ(discovery.calls, 1);
}

TEST(ScreenCapturePortsTest, PermissionRequestReturnsResultInsteadOfNativeError) {
    PermissionStub permission;
    std::optional<Result<ScreenCapturePermissionStatus>> result;

    permission.request([&result](auto value) { result.emplace(std::move(value)); });

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->hasValue());
    EXPECT_EQ(result->value(), ScreenCapturePermissionStatus::Granted);
    EXPECT_EQ(permission.status(), ScreenCapturePermissionStatus::Granted);
    EXPECT_EQ(permission.requests, 1);
}

static_assert(!std::is_copy_constructible_v<IScreenCaptureDiscovery>);
static_assert(!std::is_copy_constructible_v<IScreenCapturePermission>);

}  // namespace

