#pragma once

#include "edit_engine/IEditEngine.h"
#include "app/IExportDestinationResolver.h"

#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace creator::app {

class ExportWorker;

class ExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int state READ state NOTIFY progressChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY sourceChanged)
    Q_PROPERTY(quint32 maximumExportHeight READ maximumExportHeight NOTIFY constraintsChanged)
    Q_PROPERTY(bool foregroundExportRequired READ foregroundExportRequired NOTIFY constraintsChanged)
    Q_PROPERTY(bool exportAllowed READ exportAllowed NOTIFY constraintsChanged)
    Q_PROPERTY(bool applicationActive READ applicationActive NOTIFY constraintsChanged)

public:
    explicit ExportController(std::unique_ptr<edit_engine::IEditEngine> engine,
                              QObject* parent = nullptr);
    ExportController(std::unique_ptr<edit_engine::IEditEngine> engine,
                     std::unique_ptr<IExportDestinationResolver> destinations,
                     QObject* parent = nullptr);
    ~ExportController() override;

    void setRequest(edit_engine::RenderRequest request);
    void setSource(domain::ProjectId projectId,
                   edit_engine::TimelineSnapshot snapshot);
    void clearSource();
    void setResourceConstraints(std::uint32_t maximumExportHeight,
                                bool foregroundExportRequired,
                                bool exportAllowed);
    void setApplicationActive(bool active);

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool canCancel() const noexcept;
    [[nodiscard]] double progress() const noexcept { return progress_; }
    [[nodiscard]] int state() const noexcept { return state_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] bool ready() const noexcept { return source_.has_value(); }
    [[nodiscard]] quint32 maximumExportHeight() const noexcept {
        return maximumExportHeight_;
    }
    [[nodiscard]] bool foregroundExportRequired() const noexcept {
        return foregroundExportRequired_;
    }
    [[nodiscard]] bool exportAllowed() const noexcept { return exportAllowed_; }
    [[nodiscard]] bool applicationActive() const noexcept {
        return applicationActive_;
    }

    Q_INVOKABLE void startExport();
    Q_INVOKABLE void exportTo(const QUrl& destination,
                              const QString& presetId,
                              bool replaceExisting);
    Q_INVOKABLE void cancelExport();

signals:
    void busyChanged();
    void progressChanged();
    void statusMessageChanged();
    void sourceChanged();
    void constraintsChanged();
    void exportFinished(bool success);

private:
    void setStatus(QString message);
    [[nodiscard]] bool constraintsPermitStart();

    struct Source final {
        domain::ProjectId projectId;
        edit_engine::TimelineSnapshot snapshot;
    };

    std::shared_ptr<std::atomic_bool> cancellationRequested_;
    QThread workerThread_;
    ExportWorker* worker_{};
    std::unique_ptr<IExportDestinationResolver> destinations_;
    ExportPublishAction publishAction_;
    std::optional<edit_engine::RenderRequest> request_;
    std::optional<Source> source_;
    bool busy_{};
    double progress_{};
    int state_{static_cast<int>(edit_engine::RenderJobState::Pending)};
    QString statusMessage_;
    std::uint32_t maximumExportHeight_{std::numeric_limits<std::uint32_t>::max()};
    bool foregroundExportRequired_{};
    bool exportAllowed_{true};
    bool applicationActive_{true};
    bool constraintCancellation_{};
};

}  // namespace creator::app
