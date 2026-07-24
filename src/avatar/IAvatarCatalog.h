#pragma once

#include "avatar/AvatarAssetManifest.h"
#include "core/Result.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace creator::avatar {

/// Read-only authority for discovering verified installed avatar assets.
///
/// Implementations must fail closed when installed metadata is corrupt. A
/// returned payload root is authority only beneath an implementation-owned,
/// immutable catalog after that implementation has reverified its contents.
/// The returned path must not be cached past the lifetime of the catalog
/// instance that supplied it: destroying the instance releases the retained
/// filesystem authority that makes the path trustworthy.
class IAvatarCatalog {
public:
    virtual ~IAvatarCatalog() = default;

    [[nodiscard]] virtual core::Result<std::vector<AvatarAssetManifest>>
    list() const = 0;
    [[nodiscard]] virtual core::Result<AvatarAssetManifest> find(
        const AvatarAssetId& id, std::string_view version) const = 0;
    [[nodiscard]] virtual core::Result<std::filesystem::path> payloadRoot(
        const AvatarAssetId& id, std::string_view version) const = 0;

protected:
    IAvatarCatalog() = default;
};

}  // namespace creator::avatar
