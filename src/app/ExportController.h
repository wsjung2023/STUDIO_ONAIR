#pragma once

#include "edit_engine/IEditEngine.h"

#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>

#include <atomic>
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
    /// True when the current project's canvas is portrait (9:16), so the export
    /// UI can offer the matching vertical presets.
    Q_PROPERTY(bool portrait READ portrait NOTIFY sourceChanged)
    /// User-controlled export loudness normalization (음량 표준화) to a broadcast
    /// target (-14 LUFS). Off by default; persisted in QSettings and read by the
    /// export engine per render. This is the "control/custom" surface for the
    /// otherwise-raw exported audio.
    Q_PROPERTY(bool loudnessNormalization READ loudnessNormalization WRITE
                   setLoudnessNormalization NOTIFY loudnessNormalizationChanged)

public:
    explicit ExportController(std::unique_ptr<edit_engine::IEditEngine> engine,
                              QObject* parent = nullptr);
    ~ExportController() override;

    void setRequest(edit_engine::RenderRequest request);
    void setSource(domain::ProjectId projectId,
                   edit_engine::TimelineSnapshot snapshot);
    void clearSource();

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool canCancel() const noexcept;
    [[nodiscard]] double progress() const noexcept { return progress_; }
    [[nodiscard]] int state() const noexcept { return state_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] bool ready() const noexcept { return source_.has_value(); }
    [[nodiscard]] bool portrait() const noexcept {
        return source_.has_value() &&
               source_->snapshot.canvasHeight > source_->snapshot.canvasWidth;
    }
    [[nodiscard]] bool loudnessNormalization() const;
    void setLoudnessNormalization(bool enabled);

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
    void loudnessNormalizationChanged();
    void exportFinished(bool success);

private:
    void setStatus(QString message);

    struct Source final {
        domain::ProjectId projectId;
        edit_engine::TimelineSnapshot snapshot;
    };

    std::shared_ptr<std::atomic_bool> cancellationRequested_;
    QThread workerThread_;
    ExportWorker* worker_{};
    std::optional<edit_engine::RenderRequest> request_;
    std::optional<Source> source_;
    bool busy_{};
    double progress_{};
    int state_{static_cast<int>(edit_engine::RenderJobState::Pending)};
    QString statusMessage_;
};

}  // namespace creator::app
