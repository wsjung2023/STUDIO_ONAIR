#include "app/ProjectController.h"

#include "app/ProjectWorker.h"
#include "app/RecentProjectRegistry.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"
#include "project_store/ProjectPackageStore.h"

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>

#include <optional>
#include <utility>

namespace creator::app {
namespace {

std::filesystem::path productionRegistryPath() {
    const QDir config{QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)};
    return pathFromQString(config.filePath(QStringLiteral("recent-projects.json")));
}

}  // namespace

ProjectController::ProjectController(QObject* parent)
    : ProjectController(std::make_unique<project_store::ProjectPackageStore>(),
                        productionRegistryPath(), true, parent) {}

ProjectController::ProjectController(
    std::unique_ptr<project_store::IProjectPackageStore> store, QObject* parent)
    : ProjectController(std::move(store), productionRegistryPath(), true, parent) {}

ProjectController::ProjectController(
    std::unique_ptr<project_store::IProjectPackageStore> store,
    std::filesystem::path registryPath, bool refreshOnStartup, QObject* parent)
    : QObject(parent),
      worker_(new ProjectWorker{std::move(store), RecentProjectRegistry{std::move(registryPath)}}) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ProjectWorker::openFinished, this,
            &ProjectController::handleOpenFinished);
    connect(worker_, &ProjectWorker::recoveryFinished, this,
            [this](bool success, const QVariantMap& recovery, const QString& error) {
                setBusy(false);
                if (!success) {
                    setStatus(error);
                    return;
                }
                project_ = std::move(pendingProject_);
                pendingProject_.clear();
                recoveries_.clear();
                emit recoveriesChanged();
                emit projectChanged();
                emit projectOpened();
                setStatus(QStringLiteral(
                              "Recovered %1 ready segments; quarantined %2 interrupted and %3 orphan part files")
                              .arg(recovery.value(QStringLiteral("readySegments"))
                                       .toULongLong())
                              .arg(recovery.value(QStringLiteral("quarantinedParts"))
                                       .toULongLong())
                              .arg(recovery.value(QStringLiteral("orphanParts"))
                                       .toULongLong()));
            });
    connect(worker_, &ProjectWorker::recentScanFinished, this,
            [this](const QVariantList& recent, const QVariantList& recovery,
                   const QString& error) {
                recentProjects_ = recent;
                recoveries_ = recovery;
                emit recentProjectsChanged();
                emit recoveriesChanged();
                if (!error.isEmpty()) setStatus(error);
                if (!recoveries_.isEmpty()) emit recoveryRequired();
            });
    connect(worker_, &ProjectWorker::recordingCommandFinished, this,
            [this](quint64 commandId, bool success, int errorCode,
                   const QString& error) {
                if (success) {
                    finishRecordingCommand(commandId, core::ok());
                } else {
                    finishRecordingCommand(
                        commandId,
                        core::AppError{static_cast<core::ErrorCode>(errorCode),
                                       error.toStdString()});
                }
            });
    workerThread_.start();
    if (refreshOnStartup) refreshRecentProjects();
}

ProjectController::~ProjectController() {
    disconnect(worker_, nullptr, this, nullptr);
    workerThread_.quit();
    workerThread_.wait();
    auto pending = std::move(recordingCompletions_);
    for (auto& [commandId, completion] : pending) {
        static_cast<void>(commandId);
        if (completion) {
            completion(core::AppError{
                core::ErrorCode::InvalidState,
                "project controller stopped before recording persistence completed"});
        }
    }
}

QString ProjectController::projectName() const {
    return project_.value(QStringLiteral("name")).toString();
}

QString ProjectController::projectId() const {
    return project_.value(QStringLiteral("projectId")).toString();
}

QUrl ProjectController::projectUrl() const {
    return project_.value(QStringLiteral("url")).toUrl();
}

void ProjectController::setBusy(bool value) {
    if (busy_ == value) return;
    busy_ = value;
    emit busyChanged();
}

void ProjectController::setStatus(QString value) {
    if (statusMessage_ == value) return;
    statusMessage_ = std::move(value);
    emit statusMessageChanged();
}

bool ProjectController::rejectIfBusy() {
    if (!busy_) return false;
    setStatus(QStringLiteral("A project operation is already running"));
    return true;
}

std::optional<std::filesystem::path> ProjectController::localPath(const QUrl& url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Project URL must refer to a local file"));
        return std::nullopt;
    }
    return pathFromQString(url.toLocalFile());
}

void ProjectController::createProject(const QUrl& packageUrl, const QString& displayName) {
    if (rejectIfBusy()) return;
    auto path = localPath(packageUrl);
    if (!path.has_value()) return;
    if (QString::compare(qStringFromPath(path->extension()), QStringLiteral(".cstudio"),
                         Qt::CaseInsensitive) != 0) {
        *path += ".cstudio";
    }
    setBusy(true);
    setStatus(QString{});
    const std::string name = displayName.toStdString();
    QMetaObject::invokeMethod(worker_,
                              [worker = worker_, path = std::move(*path), name] {
                                  worker->createProject(path, name);
                              },
                              Qt::QueuedConnection);
}

void ProjectController::openProject(const QUrl& packageUrl) {
    if (rejectIfBusy()) return;
    auto path = localPath(packageUrl);
    if (!path.has_value()) return;
    setBusy(true);
    setStatus(QString{});
    QMetaObject::invokeMethod(worker_,
                              [worker = worker_, path = std::move(*path)] {
                                  worker->openProject(path);
                              },
                              Qt::QueuedConnection);
}

void ProjectController::handleOpenFinished(bool success, QVariantMap project,
                                           QVariantList recoveries, QString errorMessage) {
    setBusy(false);
    if (!success) {
        setStatus(std::move(errorMessage));
        return;
    }
    setStatus(errorMessage);
    recoveries_ = std::move(recoveries);
    emit recoveriesChanged();
    if (!recoveries_.isEmpty()) {
        pendingProject_ = std::move(project);
        emit recoveryRequired();
        return;
    }
    project_ = std::move(project);
    pendingProject_.clear();
    emit projectChanged();
    emit projectOpened();
}

void ProjectController::recoverSession(const QString& sessionIdText) {
    if (rejectIfBusy()) return;
    auto id = domain::SessionId::create(sessionIdText.toStdString());
    if (!id.hasValue()) {
        setStatus(QString::fromStdString(id.error().message()));
        return;
    }
    if (pendingProject_.isEmpty() && project_.isEmpty()) {
        for (const auto& value : recoveries_) {
            const QVariantMap candidate = value.toMap();
            if (candidate.value(QStringLiteral("sessionId")).toString() == sessionIdText) {
                pendingProject_ = QVariantMap{
                    {QStringLiteral("name"), candidate.value(QStringLiteral("projectName"))},
                    {QStringLiteral("url"), candidate.value(QStringLiteral("projectUrl"))}};
                break;
            }
        }
    }
    if (pendingProject_.isEmpty() && project_.isEmpty()) {
        setStatus(QStringLiteral("No project is selected for recovery"));
        return;
    }
    const QVariantMap& selected = pendingProject_.isEmpty() ? project_ : pendingProject_;
    const auto path = pathFromQString(selected.value(QStringLiteral("url")).toUrl().toLocalFile());
    setBusy(true);
    QMetaObject::invokeMethod(worker_,
                              [worker = worker_, path, id = std::move(id).value()] {
                                  worker->recoverSession(path, id);
                              },
                              Qt::QueuedConnection);
}

void ProjectController::leaveRecoveryForLater() {
    pendingProject_.clear();
    emit recoveryDeferred();
}

void ProjectController::refreshRecentProjects() {
    QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->refreshRecentProjects(); },
                              Qt::QueuedConnection);
}

std::optional<std::filesystem::path> ProjectController::recordingPackagePath() const {
    if (project_.isEmpty()) return std::nullopt;
    const QUrl url = project_.value(QStringLiteral("url")).toUrl();
    if (!url.isLocalFile()) return std::nullopt;
    return pathFromQString(url.toLocalFile());
}

void ProjectController::failRecordingCommandAsync(Completion completion) {
    const quint64 commandId = retainRecordingCompletion(std::move(completion));
    QTimer::singleShot(0, this, [this, commandId] {
        finishRecordingCommand(
            commandId,
            core::AppError{core::ErrorCode::InvalidState,
                           "no project is open for recording"});
    });
}

quint64 ProjectController::retainRecordingCompletion(Completion completion) {
    const quint64 commandId = nextRecordingCommandId_++;
    recordingCompletions_.emplace(commandId, std::move(completion));
    return commandId;
}

void ProjectController::finishRecordingCommand(quint64 commandId,
                                               core::Result<void> result) {
    auto found = recordingCompletions_.find(commandId);
    if (found == recordingCompletions_.end()) return;
    auto completion = std::move(found->second);
    recordingCompletions_.erase(found);
    if (completion) completion(std::move(result));
}

void ProjectController::begin(const domain::SessionId& sessionId,
                              core::TimestampNs startedAt, Completion completion) {
    auto path = recordingPackagePath();
    if (!path) {
        failRecordingCommandAsync(std::move(completion));
        return;
    }
    const quint64 commandId = retainRecordingCompletion(std::move(completion));
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, commandId, path = std::move(*path), sessionId, startedAt] {
            worker->beginRecording(commandId, path, sessionId, startedAt);
        },
        Qt::QueuedConnection);
}

void ProjectController::complete(const domain::RecordingSession& session,
                                 Completion completion) {
    auto path = recordingPackagePath();
    if (!path) {
        failRecordingCommandAsync(std::move(completion));
        return;
    }
    const quint64 commandId = retainRecordingCompletion(std::move(completion));
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, commandId, path = std::move(*path), session] {
            worker->completeRecording(commandId, path, session);
        },
        Qt::QueuedConnection);
}

void ProjectController::abort(const domain::SessionId& sessionId, std::string reason,
                              Completion completion) {
    auto path = recordingPackagePath();
    if (!path) {
        failRecordingCommandAsync(std::move(completion));
        return;
    }
    const quint64 commandId = retainRecordingCompletion(std::move(completion));
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, commandId, path = std::move(*path), sessionId,
         reason = std::move(reason)] {
            worker->abortRecording(commandId, path, sessionId, reason);
        },
        Qt::QueuedConnection);
}

}  // namespace creator::app
