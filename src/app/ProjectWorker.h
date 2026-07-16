#pragma once

#include "app/RecentProjectRegistry.h"
#include "project_store/IProjectPackageStore.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <memory>
#include <string>

namespace creator::app {

class ProjectWorker final : public QObject {
    Q_OBJECT
public:
    ProjectWorker(std::unique_ptr<project_store::IProjectPackageStore> store,
                  RecentProjectRegistry registry);

    void createProject(std::filesystem::path path, std::string name);
    void openProject(std::filesystem::path path);
    void recoverSession(std::filesystem::path path, domain::SessionId sessionId);
    void refreshRecentProjects();

signals:
    void openFinished(bool success, QVariantMap project, QVariantList recoveries,
                      QString errorMessage);
    void recoveryFinished(bool success, QVariantMap recovery, QString errorMessage);
    void recentScanFinished(QVariantList recentProjects, QVariantList recoveries,
                            QString errorMessage);
    void recordingCommandFinished(quint64 commandId, bool success, QString errorMessage);

private:
    void publishOpen(core::Result<project_store::OpenProjectResult> result);

    std::unique_ptr<project_store::IProjectPackageStore> store_;
    RecentProjectRegistry registry_;
};

}  // namespace creator::app
