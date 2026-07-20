#include "platform_release/EntitlementPolicy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {
using creator::platform_release::EntitlementAssertion;
using creator::platform_release::EntitlementClockState;
using creator::platform_release::EntitlementPolicy;
using creator::platform_release::EntitlementPolicyConfig;
using creator::platform_release::EntitlementState;

constexpr std::int64_t kNow = 2'000'000;
constexpr std::int64_t kDay = 86'400;

EntitlementPolicy policy(bool communityBuild = false) {
    return EntitlementPolicy{{.productId = "creator-studio-pro",
                              .offlineGraceSeconds = 7 * kDay,
                              .communityBuild = communityBuild}};
}

EntitlementAssertion assertion() {
    return {.productId = "creator-studio-pro",
            .validUntilUtcSeconds = kNow + kDay,
            .revoked = false};
}

EntitlementClockState clock() {
    return {.trustedNowUtcSeconds = kNow,
            .lastOnlineCheckUtcSeconds = kNow - kDay,
            .lastObservedUtcSeconds = kNow - 1};
}

TEST(EntitlementPolicyTest, AllowsActiveMatchingAssertion) {
    const auto decision = policy().evaluate(assertion(), clock());
    EXPECT_EQ(decision.state, EntitlementState::Active);
    EXPECT_TRUE(decision.commercialFeaturesAllowed);
    EXPECT_EQ(decision.reason, "verified-entitlement-active");
}

TEST(EntitlementPolicyTest, ExpiredWithoutOnlineStateIsUnavailable) {
    auto expired = assertion();
    expired.validUntilUtcSeconds = kNow - 1;
    auto noOnlineState = clock();
    noOnlineState.lastOnlineCheckUtcSeconds.reset();
    const auto decision = policy().evaluate(expired, noOnlineState);
    EXPECT_EQ(decision.state, EntitlementState::Unavailable);
    EXPECT_FALSE(decision.commercialFeaturesAllowed);
    EXPECT_EQ(decision.reason, "entitlement-expired");
}

TEST(EntitlementPolicyTest, AllowsExpiredAssertionInsideOfflineGrace) {
    auto expired = assertion();
    expired.validUntilUtcSeconds = kNow - 1;
    const auto decision = policy().evaluate(expired, clock());
    EXPECT_EQ(decision.state, EntitlementState::OfflineGrace);
    EXPECT_TRUE(decision.commercialFeaturesAllowed);
    EXPECT_EQ(decision.reason, "offline-grace-active");
}

TEST(EntitlementPolicyTest, RejectsExpiredAssertionAfterOfflineGrace) {
    auto expired = assertion();
    expired.validUntilUtcSeconds = kNow - 1;
    auto stale = clock();
    stale.lastOnlineCheckUtcSeconds = kNow - (8 * kDay);
    const auto decision = policy().evaluate(expired, stale);
    EXPECT_EQ(decision.state, EntitlementState::Unavailable);
    EXPECT_EQ(decision.reason, "offline-grace-expired");
}

TEST(EntitlementPolicyTest, RevocationOverridesValidityAndGrace) {
    auto revoked = assertion();
    revoked.revoked = true;
    const auto decision = policy().evaluate(revoked, clock());
    EXPECT_EQ(decision.state, EntitlementState::Unavailable);
    EXPECT_EQ(decision.reason, "entitlement-revoked");
}

TEST(EntitlementPolicyTest, RejectsAssertionForAnotherProduct) {
    auto wrongProduct = assertion();
    wrongProduct.productId = "another-product";
    const auto decision = policy().evaluate(wrongProduct, clock());
    EXPECT_EQ(decision.state, EntitlementState::Unavailable);
    EXPECT_EQ(decision.reason, "entitlement-product-mismatch");
}

TEST(EntitlementPolicyTest, ClockRollbackNeverExtendsOfflineGrace) {
    auto expired = assertion();
    expired.validUntilUtcSeconds = kNow - 1;
    auto rolledBack = clock();
    rolledBack.trustedNowUtcSeconds = kNow - (2 * kDay);
    rolledBack.lastObservedUtcSeconds = kNow;
    const auto decision = policy().evaluate(expired, rolledBack);
    EXPECT_EQ(decision.state, EntitlementState::Unavailable);
    EXPECT_EQ(decision.reason, "trusted-clock-rollback");
}

TEST(EntitlementPolicyTest, CommunityBuildIsExplicitlyAllowedWithoutPurchaseClaim) {
    EntitlementAssertion empty;
    EntitlementClockState current{.trustedNowUtcSeconds = kNow};
    const auto decision = policy(true).evaluate(empty, current);
    EXPECT_EQ(decision.state, EntitlementState::Active);
    EXPECT_TRUE(decision.commercialFeaturesAllowed);
    EXPECT_EQ(decision.reason, "community-development-build");
}

}  // namespace
