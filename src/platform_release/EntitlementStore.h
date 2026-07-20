#pragma once

#include "core/Result.h"
#include "platform_release/EntitlementPolicy.h"
#include "platform_release/IReceiptVerifier.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace creator::platform_release {

struct EntitlementStateRecord final {
    std::string providerId;
    EntitlementAssertion assertion;
    std::int64_t lastOnlineCheckUtcSeconds{};
    std::int64_t lastObservedUtcSeconds{};

    friend bool operator==(const EntitlementStateRecord&,
                           const EntitlementStateRecord&) = default;
};

class EntitlementStore final {
public:
    explicit EntitlementStore(std::filesystem::path statePath)
        : statePath_(std::move(statePath)) {}

    [[nodiscard]] core::Result<EntitlementAssertion> verifyReceipt(
        std::string_view providerId, std::span<const std::byte> opaqueReceipt,
        const IReceiptVerifier& verifier) const;
    [[nodiscard]] core::Result<void> write(
        const EntitlementStateRecord& record) const;
    [[nodiscard]] core::Result<EntitlementStateRecord> read() const;

private:
    std::filesystem::path statePath_;
};

}  // namespace creator::platform_release
