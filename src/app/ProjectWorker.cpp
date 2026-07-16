#include "app/ProjectWorker.h"

#include "core/Utc.h"

#include <QUrl>

#include <filesystem>
#include <utility>

namespace creator::app {
namespace {

QVariantMap recoveryMap(const project_store::RecoveryCandidate& candidate) {
    return QVariantMap{{QStringLiteral("sessionId"),
                        QString::fromStdString(candidate.sessionId.value())},
                       {QStringLiteral("projectName"),
                        QString::fromStdString(candidate.projectName)},
                       {QStringLiteral("projectUrl"),
                        QUrl::fromLocalFile(qStringFromPath(candidate.packagePath))},
                       {QStringLiteral("createdAt"),
                        QString::fromStdString(candidate.createdAt.toRfc3339())},
                       {QStringLiteral("readySegments"),
                        static_cast<qulonglong>(candidate.readySegments)},
                       {QStringLiteral("writingSegments"),
                        static_cast<qulonglong>(candidate.writingSegments)}};
}

QVariantList recoveryMaps(const std::vector<project_store::RecoveryCandidate>& candidates) {
    QVariantList result;
    for (const auto& candidate : candidates) result.append(recoveryMap(candidate));
    return result;
}

QVariantMap projectMap(const project_store::ProjectPackage& package) {
    return QVariantMap{{QStringLiteral("name"),
                        QString::fromStdString(package.manifest.name)},
                       {QStringLiteral("url"),
                        QUrl::fromLocalFile(qStringFromPath(package.path))}};
}

}  // namespace

ProjectWorker::ProjectWorker(std::unique_ptr<project_store::IProjectPackageStore> store,
                             RecentProjectRegistry registry)
    : store_(std::move(store)), registry_(std::move(registry)) {}

void ProjectWorker::createProject(std::filesystem::path path, std::string name) {
    publishOpen(store_->create(path, name));
}

void ProjectWorker::openProject(std::filesystem::path path) {
    publishOpen(store_->open(path));
}

void ProjectWorker::publishOpen(core::Result<project_store::OpenProjectResult> result) {
    if (!result.hasValue()) {
        emit openFinished(false, {}, {}, QString::fromStdString(result.error().message()));
        return;
    }
    auto remembered = registry_.remember(result.value().package.path, core::Utc::now());
    const QString warning = remembered.hasValue()
                                ? QString{}
                                : QString::fromStdString(remembered.error().message());
    emit openFinished(true, projectMap(result.value().package),
                      recoveryMaps(result.value().recoveryCandidates), warning);
}

void ProjectWorker::recoverSession(std::filesystem::path path, domain::SessionId sessionId) {
    auto recovered = store_->recover(path, sessionId, core::Utc::now());
    if (!recovered.hasValue()) {
        emit recoveryFinished(false, {}, QString::fromStdString(recovered.error().message()));
        return;
    }
    emit recoveryFinished(
        true,
        QVariantMap{{QStringLiteral("sessionId"),
                     QString::fromStdString(recovered.value().sessionId.value())},
                    {QStringLiteral("readySegments"),
                     static_cast<qulonglong>(recovered.value().readySegments)},
                    {QStringLiteral("failedSegments"),
                     static_cast<qulonglong>(recovered.value().failedSegments)},
                    {QStringLiteral("quarantinedParts"),
                     static_cast<qulonglong>(recovered.value().quarantinedParts)},
                    {QStringLiteral("orphanParts"),
                     static_cast<qulonglong>(recovered.value().orphanParts)}},
        {});
}

void ProjectWorker::refreshRecentProjects() {
    auto loaded = registry_.load();
    if (!loaded.hasValue()) {
        emit recentScanFinished({}, {}, QString::fromStdString(loaded.error().message()));
        return;
    }
    QVariantList rows;
    QVariantList recoveries;
    for (const auto& recent : loaded.value()) {
        std::error_code ec;
        const bool available = std::filesystem::exists(recent.path, ec) && !ec;
        QVariantMap row{{QStringLiteral("projectUrl"),
                         QUrl::fromLocalFile(qStringFromPath(recent.path))},
                        {QStringLiteral("projectName"), recent.path.stem().empty()
                                                                ? QString{}
                                                                : qStringFromPath(recent.path.stem())},
                        {QStringLiteral("lastOpenedAt"),
                         QString::fromStdString(recent.lastOpenedAt.toRfc3339())},
                        {QStringLiteral("available"), available}};
        if (available) {
            auto opened = store_->open(recent.path);
            if (opened.hasValue()) {
                row[QStringLiteral("projectName")] =
                    QString::fromStdString(opened.value().package.manifest.name);
                for (const auto& value : recoveryMaps(opened.value().recoveryCandidates)) {
                    recoveries.append(value);
                }
            }
        }
        rows.append(row);
    }
    emit recentScanFinished(rows, recoveries, {});
}

void ProjectWorker::beginRecording(quint64 commandId, std::filesystem::path path,
                                   domain::SessionId sessionId,
                                   core::TimestampNs startedAt) {
    auto result = store_->beginRecording(path, sessionId, startedAt, core::Utc::now());
    emit recordingCommandFinished(
        commandId, result.hasValue(),
        result.hasValue() ? 0 : static_cast<int>(result.error().code()),
        result.hasValue() ? QString{} : QString::fromStdString(result.error().message()));
}

void ProjectWorker::completeRecording(quint64 commandId, std::filesystem::path path,
                                      domain::RecordingSession session) {
    auto result = store_->completeRecording(path, session, core::Utc::now());
    emit recordingCommandFinished(
        commandId, result.hasValue(),
        result.hasValue() ? 0 : static_cast<int>(result.error().code()),
        result.hasValue() ? QString{} : QString::fromStdString(result.error().message()));
}

void ProjectWorker::abortRecording(quint64 commandId, std::filesystem::path path,
                                   domain::SessionId sessionId, std::string reason) {
    auto result = store_->abortRecording(path, sessionId, reason, core::Utc::now());
    emit recordingCommandFinished(
        commandId, result.hasValue(),
        result.hasValue() ? 0 : static_cast<int>(result.error().code()),
        result.hasValue() ? QString{} : QString::fromStdString(result.error().message()));
}

}  // namespace creator::app
