#pragma once

#include "avatar_pack_adapter/AvatarPackStaging.h"

#include <utility>

namespace creator::avatar_pack_adapter::detail {

template <typename SourceFlush, typename DestinationFlush>
[[nodiscard]] PromotionOutcome confirmPromotionDurability(
    SourceFlush&& sourceFlush, DestinationFlush&& destinationFlush) noexcept {
    bool sourceDurable = false;
    bool destinationDurable = false;
    try {
        sourceDurable = std::forward<SourceFlush>(sourceFlush)();
    } catch (...) {
    }
    try {
        destinationDurable =
            std::forward<DestinationFlush>(destinationFlush)();
    } catch (...) {
    }
    return sourceDurable && destinationDurable
               ? PromotionOutcome::Durable
               : PromotionOutcome::Indeterminate;
}

}  // namespace creator::avatar_pack_adapter::detail
