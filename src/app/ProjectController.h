#pragma once

#include "app/IRecordingPersistence.h"
#include "project_store/IProjectPackageStore.h"

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>

namespace creator::app {

class ProjectWorker;

class ProjectController final : public QObject, public IRecordingPersistence {
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
                      QObject* parent);
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
    [[nodiscard]] std::optional<std::filesystem::path> recordingPackagePath() const;

    Q_INVOKABLE void createProject(const QUrl& packageUrl, const QString& displayName);
    Q_INVOKABLE void openProject(const QUrl& packageUrl);
    Q_INVOKABLE void recoverSession(const QString& sessionId);
    Q_INVOKABLE void leaveRecoveryForLater();
    Q_INVOKABLE void refreshRecentProjects();

    void begin(const domain::SessionId& sessionId, core::TimestampNs startedAt,
               Completion completion) override;
    void complete(const domain::RecordingSession& session,
                  Completion completion) override;
    void abort(const domain::SessionId& sessionId, std::string reason,
               Completion completion) override;

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
    void failRecordingCommandAsync(Completion completion);
    [[nodiscard]] quint64 retainRecordingCompletion(Completion completion);
    void finishRecordingCommand(quint64 commandId, core::Result<void> result);

    QThread workerThread_;
    ProjectWorker* worker_{};
    bool busy_{false};
    QVariantMap project_;
    QVariantMap pendingProject_;
    QVariantList recentProjects_;
    QVariantList recoveries_;
    QString statusMessage_;
    quint64 nextRecordingCommandId_{1};
    std::unordered_map<quint64, Completion> recordingCompletions_;
};

}  // namespace creator::app
