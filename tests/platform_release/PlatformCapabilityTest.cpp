#include "platform_release/PlatformCapability.h"
#include "platform_release/PlatformCapabilityRegistry.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::platform_release::CapabilityState;
using creator::platform_release::PlatformCapability;
using creator::platform_release::PlatformCapabilityRegistry;
using creator::platform_release::PlatformKind;

TEST(PlatformCapabilityTest, AvailableCapabilityNeedsNoUnavailableReason) {
    const auto capability = PlatformCapability::create(
        "screen-capture", CapabilityState::Available, "");

    ASSERT_TRUE(capability.hasValue());
    EXPECT_EQ(capability.value().id(), "screen-capture");
    EXPECT_EQ(capability.value().state(), CapabilityState::Available);
}

TEST(PlatformCapabilityTest, UnavailableCapabilityNeedsExplanation) {
    const auto capability = PlatformCapability::create(
        "system-audio", CapabilityState::Unavailable, "");

    ASSERT_FALSE(capability.hasValue());
    EXPECT_EQ(capability.error().code(), ErrorCode::InvalidArgument);
}

TEST(PlatformCapabilityTest, RejectsUnsafeOrEmptyIdentifier) {
    EXPECT_FALSE(PlatformCapability::create("", CapabilityState::Available, "").hasValue());
    EXPECT_FALSE(PlatformCapability::create("screen capture", CapabilityState::Available, "").hasValue());
    EXPECT_FALSE(PlatformCapability::create("../capture", CapabilityState::Available, "").hasValue());
}

TEST(PlatformCapabilityRegistryTest, RejectsDuplicateCapabilityIdentifiers) {
    const auto first = PlatformCapability::create(
        "screen-capture", CapabilityState::Available, "").value();
    const auto duplicate = PlatformCapability::create(
        "screen-capture", CapabilityState::PermissionRequired,
        "Screen capture permission is required.").value();

    const auto registry = PlatformCapabilityRegistry::create(
        PlatformKind::Windows, {first, duplicate});

    ASSERT_FALSE(registry.hasValue());
    EXPECT_EQ(registry.error().code(), ErrorCode::AlreadyExists);
}

TEST(PlatformCapabilityRegistryTest, LookupReturnsImmutableCapabilitySnapshot) {
    const auto screen = PlatformCapability::create(
        "screen-capture", CapabilityState::PermissionRequired,
        "Screen capture permission is required.").value();
    const auto registry = PlatformCapabilityRegistry::create(
        PlatformKind::Windows, {screen});

    ASSERT_TRUE(registry.hasValue());
    const auto found = registry.value().find("screen-capture");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->state(), CapabilityState::PermissionRequired);
    EXPECT_EQ(registry.value().find("missing-capability"), nullptr);
}

TEST(PlatformCapabilityRegistryTest, AndroidIsDeclaredAsFirstClassRecordingTarget) {
    const auto registry = PlatformCapabilityRegistry::defaultsFor(PlatformKind::Android);

    const auto screen = registry.find("screen-capture");
    ASSERT_NE(screen, nullptr);
    EXPECT_EQ(screen->state(), CapabilityState::PermissionRequired);

    const auto editing = registry.find("timeline-editing");
    ASSERT_NE(editing, nullptr);
    EXPECT_EQ(editing->state(), CapabilityState::Available);
}

}  // namespace
