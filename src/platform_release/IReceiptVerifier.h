#pragma once

#include "core/Result.h"
#include "platform_release/EntitlementPolicy.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace creator::platform_release {

/// Provider adapter port. Opaque receipt bytes cross this boundary transiently
/// and are never part of EntitlementStore's persistent representation.
class IReceiptVerifier {
public:
    virtual ~IReceiptVerifier() = default;
    [[nodiscard]] virtual core::Result<EntitlementAssertion> verify(
        std::string_view providerId,
        std::span<const std::byte> opaqueReceipt) const = 0;
};

}  // namespace creator::platform_release
