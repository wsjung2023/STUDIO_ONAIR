#pragma once

#include "core/Result.h"
#include "domain/EditCommand.h"
#include "domain/MediaAsset.h"

#include <functional>
#include <memory>

namespace creator::project_store::internal {

class EditCommandCodec final {
public:
    using AssetLoader = std::function<core::Result<domain::MediaAsset>(
        const domain::AssetId&)>;

    explicit EditCommandCodec(AssetLoader assetLoader)
        : assetLoader_(std::move(assetLoader)) {}

    [[nodiscard]] core::Result<std::unique_ptr<domain::IEditCommand>> decode(
        const domain::EditCommandRecord& record, bool applied) const;

private:
    AssetLoader assetLoader_;
};

}  // namespace creator::project_store::internal
