#pragma once

#include "avatar_pack_adapter/FileAvatarCatalog.h"

#include <chrono>
#include <filesystem>

namespace creator::avatar_pack_adapter::detail {

inline constexpr std::chrono::hours kAbandonedStagingAge{24};

[[nodiscard]] inline bool isAbandonedStagingEntry(
    std::filesystem::file_time_type lastWrite,
    std::filesystem::file_time_type now) noexcept {
    return lastWrite < now - kAbandonedStagingAge;
}

[[nodiscard]] inline core::Result<CatalogInstallOutcome>
reconciledInstallOutcome(PromotionOutcome promotion,
                         bool installedTargetVerified) {
    if (!installedTargetVerified) {
        return core::AppError{
            core::ErrorCode::IoFailure,
            "avatar catalog promotion requires reconciliation",
            "avatar.catalog.reconciliation-required",
            "avatar.catalog.error"};
    }
    return promotion == PromotionOutcome::Durable
               ? CatalogInstallOutcome::Installed
               : CatalogInstallOutcome::
                     InstalledDurabilityIndeterminate;
}

}  // namespace creator::avatar_pack_adapter::detail
