#pragma once

#include "project_store/IProjectPackageStore.h"

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <memory>
#include <optional>

namespace creator::app {

class ProjectWorker;

class ProjectController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasOpenProject READ hasOpenProject NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QUrl projectUrl READ projectUrl NOTIFY projectChanged)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    Q_PROPERTY(QVariantList recoveries READ recoveries NOTIFY recoveriesChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit ProjectController(QObject* parent = nullptr);
    ProjectController(std::unique_ptr<project_store::IProjectPackageStore> store,
                      std::filesystem::path registryPath, bool refreshOnStartup = true,
                      QObject* parent = nullptr);
    ~ProjectController() override;

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool hasOpenProject() const noexcept { return !project_.isEmpty(); }
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QUrl projectUrl() const;
    [[nodiscard]] QVariantList recentProjects() const { return recentProjects_; }
    [[nodiscard]] QVariantList recoveries() const { return recoveries_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }

    Q_INVOKABLE void createProject(const QUrl& packageUrl, const QString& displayName);
    Q_INVOKABLE void openProject(const QUrl& packageUrl);
    Q_INVOKABLE void recoverSession(const QString& sessionId);
    Q_INVOKABLE void leaveRecoveryForLater();
    Q_INVOKABLE void refreshRecentProjects();

signals:
    void busyChanged();
    void projectChanged();
    void recentProjectsChanged();
    void recoveriesChanged();
    void statusMessageChanged();
    void projectOpened();
    void recoveryRequired();
    void recoveryDeferred();

private:
    void setBusy(bool value);
    void setStatus(QString value);
    [[nodiscard]] bool rejectIfBusy();
    [[nodiscard]] std::optional<std::filesystem::path> localPath(const QUrl& url);
    void handleOpenFinished(bool success, QVariantMap project, QVariantList recoveries,
                            QString errorMessage);

    QThread workerThread_;
    ProjectWorker* worker_{};
    bool busy_{false};
    QVariantMap project_;
    QVariantMap pendingProject_;
    QVariantList recentProjects_;
    QVariantList recoveries_;
    QString statusMessage_;
};

}  // namespace creator::app
