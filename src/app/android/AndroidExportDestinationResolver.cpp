#include "app/android/AndroidExportDestinationResolver.h"

#include "app/RecentProjectRegistry.h"
#include "core/AppError.h"

#include <QJniEnvironment>
#include <QJniObject>
#include <QStandardPaths>
#include <QUuid>

#include <system_error>

namespace creator::app::android {
namespace {

constexpr const char* kActivityClass =
    "com/studioonair/creatorstudio/CreatorStudioActivity";

core::Result<void> publishContentUri(const std::filesystem::path& stagedPath,
                                     const QString& destination,
                                     bool replaceExisting) {
    const auto staged = QJniObject::fromString(
        QString::fromStdString(stagedPath.generic_string()));
    const auto contentUri = QJniObject::fromString(destination);
    auto result = QJniObject::callStaticObjectMethod(
        kActivityClass, "publishExport",
        "(Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/String;",
        staged.object<jstring>(), contentUri.object<jstring>(),
        static_cast<jboolean>(replaceExisting));
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions()) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Android could not publish the scoped-storage export"};
    }
    const auto error = result.toString();
    if (!error.isEmpty()) {
        return core::AppError{core::ErrorCode::IoFailure, error.toStdString()};
    }
    std::error_code ignored;
    std::filesystem::remove(stagedPath, ignored);
    return core::ok();
}

}  // namespace

core::Result<ResolvedExportDestination>
AndroidExportDestinationResolver::resolve(const QUrl& destination,
                                          bool replaceExisting) {
    if (destination.isLocalFile()) {
        return LocalExportDestinationResolver{}.resolve(destination,
                                                        replaceExisting);
    }
    if (destination.scheme() != QStringLiteral("content")) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Android export destination must be a content URI"};
    }
    const auto cacheRoot = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Android export cache is unavailable"};
    }
    const auto fileName = QStringLiteral("creator-studio-export-%1.mp4")
                              .arg(QUuid::createUuid().toString(
                                  QUuid::WithoutBraces));
    const auto renderPath = pathFromQString(cacheRoot + QLatin1Char('/') + fileName);
    const auto uri = destination.toString(QUrl::FullyEncoded);
    return ResolvedExportDestination{
        .renderPath = renderPath,
        .publish = [uri, replaceExisting](const std::filesystem::path& staged) {
            return publishContentUri(staged, uri, replaceExisting);
        },
    };
}

}  // namespace creator::app::android
