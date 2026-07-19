#include "app/IExportDestinationResolver.h"

#include "app/RecentProjectRegistry.h"
#include "core/AppError.h"

namespace creator::app {

core::Result<ResolvedExportDestination> LocalExportDestinationResolver::resolve(
    const QUrl& destination, bool) {
    if (!destination.isLocalFile()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "export destination must be a local MP4 file"};
    }
    return ResolvedExportDestination{
        .renderPath = pathFromQString(destination.toLocalFile()),
        .publish = {},
    };
}

}  // namespace creator::app
