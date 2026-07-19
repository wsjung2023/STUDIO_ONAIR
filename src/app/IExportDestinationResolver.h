#pragma once

#include "core/Result.h"

#include <QUrl>

#include <filesystem>
#include <functional>

namespace creator::app {

using ExportPublishAction =
    std::function<core::Result<void>(const std::filesystem::path&)>;

struct ResolvedExportDestination final {
    std::filesystem::path renderPath;
    ExportPublishAction publish;
};

/// Resolves a user-selected destination into a filesystem render target plus
/// an optional worker-thread publication step. Android content URIs use a
/// private staging file; desktop local files require no second publication.
class IExportDestinationResolver {
public:
    virtual ~IExportDestinationResolver() = default;
    [[nodiscard]] virtual core::Result<ResolvedExportDestination> resolve(
        const QUrl& destination, bool replaceExisting) = 0;
};

class LocalExportDestinationResolver final : public IExportDestinationResolver {
public:
    [[nodiscard]] core::Result<ResolvedExportDestination> resolve(
        const QUrl& destination, bool replaceExisting) override;
};

}  // namespace creator::app
