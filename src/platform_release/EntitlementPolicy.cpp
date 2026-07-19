#include "platform_release/EntitlementPolicy.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace creator::platform_release {
namespace {

bool isProductToken(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' ||
                      character == '_';
           });
}

EntitlementDecision unavailable(std::string reason) {
    return {.state = EntitlementState::Unavailable,
            .commercialFeaturesAllowed = false,
            .reason = std::move(reason)};
}

}  // namespace

EntitlementDecision EntitlementPolicy::evaluate(
    const EntitlementAssertion& assertion,
    const EntitlementClockState& clock) const {
    if (config_.communityBuild) {
        return {.state = EntitlementState::Active,
                .commercialFeaturesAllowed = true,
                .reason = "community-development-build"};
    }
    if (!isProductToken(config_.productId) || config_.offlineGraceSeconds < 0) {
        return unavailable("entitlement-policy-invalid");
    }
    if (clock.trustedNowUtcSeconds <= 0) {
        return unavailable("trusted-clock-invalid");
    }
    if ((clock.lastObservedUtcSeconds.has_value() &&
         clock.trustedNowUtcSeconds < *clock.lastObservedUtcSeconds) ||
        (clock.lastOnlineCheckUtcSeconds.has_value() &&
         clock.trustedNowUtcSeconds < *clock.lastOnlineCheckUtcSeconds)) {
        return unavailable("trusted-clock-rollback");
    }
    if (assertion.revoked) return unavailable("entitlement-revoked");
    if (assertion.productId != config_.productId) {
        return unavailable("entitlement-product-mismatch");
    }
    if (assertion.validUntilUtcSeconds <= 0) {
        return unavailable("entitlement-expired");
    }
    if (clock.trustedNowUtcSeconds <= assertion.validUntilUtcSeconds) {
        return {.state = EntitlementState::Active,
                .commercialFeaturesAllowed = true,
                .reason = "verified-entitlement-active"};
    }
    if (!clock.lastOnlineCheckUtcSeconds.has_value()) {
        return unavailable("entitlement-expired");
    }
    const auto lastOnline = *clock.lastOnlineCheckUtcSeconds;
    if (lastOnline <= 0 || lastOnline > assertion.validUntilUtcSeconds) {
        return unavailable("entitlement-expired");
    }
    if (clock.trustedNowUtcSeconds - lastOnline <= config_.offlineGraceSeconds) {
        return {.state = EntitlementState::OfflineGrace,
                .commercialFeaturesAllowed = true,
                .reason = "offline-grace-active"};
    }
    return unavailable("offline-grace-expired");
}

}  // namespace creator::platform_release
