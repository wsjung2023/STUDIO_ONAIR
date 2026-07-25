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
    Q_PROPERTY(QString projectId READ projectId NOTIFY projectChanged)
    Q_PROPERTY(QUrl projectUrl READ projectUrl NOTIFY projectChanged)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    Q_PROPERTY(QVariantList recoveries READ recoveries NOTIFY recoveriesChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    // A stable, always-writable default folder for new projects. Avoids the
    // Downloads/Documents folders when they are redirected into OneDrive, whose
    // on-demand sync intermittently revokes local write access ("이 위치에
    // 저장할 권한이 없습니다").
    Q_PROPERTY(QUrl defaultProjectFolder READ defaultProjectFolder CONSTANT)
    // The open project's canvas, surfaced so the live Studio preview can frame
    // itself to the project aspect (9:16 shorts vs 16:9). Defaults describe a
    // landscape canvas when no project is open. NOTIFY projectChanged already
    // fires on every open.
    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY projectChanged)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY projectChanged)
    Q_PROPERTY(bool portrait READ portrait NOTIFY projectChanged)

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
    [[nodiscard]] QString projectId() const;
    [[nodiscard]] QUrl projectUrl() const;
    [[nodiscard]] QVariantList recentProjects() const { return recentProjects_; }
    [[nodiscard]] QVariantList recoveries() const { return recoveries_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] QUrl defaultProjectFolder() const;
    [[nodiscard]] int canvasWidth() const;
    [[nodiscard]] int canvasHeight() const;
    [[nodiscard]] bool portrait() const;
    [[nodiscard]] std::optional<std::filesystem::path> recordingPackagePath() const;

    /// Default composition box (normalized [0,1] x/y/width/height + zOrder) for a
    /// role ("screen"/"camera"/"avatar") on the open project's PORTRAIT canvas, so
    /// the live Studio preview lays sources out exactly as the shorts export will.
    /// Returns an empty map for a landscape canvas or a non-video role, letting
    /// the caller keep its own fallback.
    Q_INVOKABLE QVariantMap defaultCompositionTransform(const QString& role) const;

    /// `portrait` selects a 9:16 (1080x1920) shorts canvas; false (default)
    /// keeps the 16:9 (1920x1080) landscape canvas and existing QML callers.
    Q_INVOKABLE void createProject(const QUrl& packageUrl,
                                   const QString& displayName,
                                   bool portrait = false);
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
