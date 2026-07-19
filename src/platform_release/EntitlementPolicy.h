#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace creator::platform_release {

enum class EntitlementState { Active, OfflineGrace, Unavailable };

struct EntitlementDecision final {
    EntitlementState state{EntitlementState::Unavailable};
    bool commercialFeaturesAllowed{false};
    std::string reason;

    friend bool operator==(const EntitlementDecision&,
                           const EntitlementDecision&) = default;
};

struct EntitlementAssertion final {
    std::string productId;
    std::int64_t validUntilUtcSeconds{};
    bool revoked{false};

    friend bool operator==(const EntitlementAssertion&,
                           const EntitlementAssertion&) = default;
};

struct EntitlementClockState final {
    std::int64_t trustedNowUtcSeconds{};
    std::optional<std::int64_t> lastOnlineCheckUtcSeconds;
    std::optional<std::int64_t> lastObservedUtcSeconds;

    friend bool operator==(const EntitlementClockState&,
                           const EntitlementClockState&) = default;
};

struct EntitlementPolicyConfig final {
    std::string productId;
    std::int64_t offlineGraceSeconds{};
    bool communityBuild{false};
};

/// Pure entitlement policy. Provider receipt verification and persistence are
/// separate boundaries; this class only evaluates already verified facts.
class EntitlementPolicy final {
public:
    explicit EntitlementPolicy(EntitlementPolicyConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] EntitlementDecision evaluate(
        const EntitlementAssertion& assertion,
        const EntitlementClockState& clock) const;

private:
    EntitlementPolicyConfig config_;
};

}  // namespace creator::platform_release
