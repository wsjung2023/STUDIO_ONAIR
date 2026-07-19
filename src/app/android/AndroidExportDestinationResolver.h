#pragma once

#include "app/IExportDestinationResolver.h"

namespace creator::app::android {

class AndroidExportDestinationResolver final : public IExportDestinationResolver {
public:
    [[nodiscard]] core::Result<ResolvedExportDestination> resolve(
        const QUrl& destination, bool replaceExisting) override;
};

}  // namespace creator::app::android
