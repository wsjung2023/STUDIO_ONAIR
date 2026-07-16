#include "app/ProjectController.h"

#include "app/ProjectWorker.h"
#include "app/RecentProjectRegistry.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"
#include "project_store/ProjectPackageStore.h"

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>

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
    std::unique_ptr<project_store::IProjectPackageStore> store,
    std::filesystem::path registryPath, bool refreshOnStartup, QObject* parent)
    : QObject(parent),
      worker_(new ProjectWorker{std::move(store), RecentProjectRegistry{std::move(registryPath)}}) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ProjectWorker::openFinished, this,
            &ProjectController::handleOpenFinished);
    connect(worker_, &ProjectWorker::recoveryFinished, this,
            [this](bool success, const QVariantMap&, const QString& error) {
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
                setStatus(QString{});
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
    workerThread_.start();
    if (refreshOnStartup) refreshRecentProjects();
}

ProjectController::~ProjectController() {
    disconnect(worker_, nullptr, this, nullptr);
    workerThread_.quit();
    workerThread_.wait();
}

QString ProjectController::projectName() const {
    return project_.value(QStringLiteral("name")).toString();
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

}  // namespace creator::app
