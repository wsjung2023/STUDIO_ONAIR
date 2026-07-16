#pragma once

#include "app/MediaBinModel.h"
#include "app/TimelineTrackModel.h"
#include "core/Timebase.h"
#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/IEditEngine.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace creator::app {

class EditorEngineWorker;
enum class EditorEngineOperation;

class EditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* mediaBinModel READ mediaBinModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineTrackModel READ timelineTrackModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool previewStale READ previewStale NOTIFY previewStaleChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qlonglong playheadNs READ playheadNs NOTIFY playheadChanged)
    Q_PROPERTY(qlonglong timelineRevision READ timelineRevision NOTIFY timelineChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QImage previewImage READ previewImage NOTIFY previewImageChanged)
    Q_PROPERTY(bool hasPreviewFrame READ hasPreviewFrame NOTIFY previewImageChanged)

public:
    explicit EditorController(std::unique_ptr<edit_engine::IEditEngine> engine,
                              QObject* parent = nullptr);
    ~EditorController() override;

    [[nodiscard]] QAbstractItemModel* mediaBinModel() noexcept {
        return &mediaBinModel_;
    }
    [[nodiscard]] QAbstractItemModel* timelineTrackModel() noexcept {
        return &timelineTrackModel_;
    }
    [[nodiscard]] bool busy() const noexcept { return pendingCommands_ > 0; }
    [[nodiscard]] bool previewStale() const noexcept { return previewStale_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] qlonglong playheadNs() const noexcept {
        return playhead_.time_since_epoch().count();
    }
    [[nodiscard]] qlonglong timelineRevision() const noexcept;
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] QImage previewImage() const { return previewImage_; }
    [[nodiscard]] bool hasPreviewFrame() const noexcept {
        return !previewImage_.isNull();
    }

    void openSession(std::vector<domain::MediaAsset> assets,
                     edit_engine::TimelineSnapshot snapshot);
    void commitTimeline(edit_engine::TimelineChangeSet change);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(qlonglong positionNs);

signals:
    void busyChanged();
    void previewStaleChanged();
    void playingChanged();
    void playheadChanged();
    void timelineChanged();
    void statusMessageChanged();
    void previewImageChanged();

private:
    struct PendingCommand final {
        quint64 generation;
        EditorEngineOperation operation;
        std::optional<core::TimestampNs> position;
        bool countsAsBusy;
        std::optional<std::int64_t> expectedRevision;
    };

    struct QueuedCommand final {
        quint64 generation;
        quint64 commandId;
        EditorEngineOperation operation;
        std::optional<core::TimestampNs> position;
        std::optional<edit_engine::TimelineSnapshot> snapshot;
        std::optional<edit_engine::TimelineChangeSet> change;
    };

    void queueLoad(edit_engine::TimelineSnapshot snapshot,
                   bool recoveryPriority = false);
    void queueUpdate(edit_engine::TimelineChangeSet change);
    void queueSimple(EditorEngineOperation operation,
                     std::optional<core::TimestampNs> position = std::nullopt);
    void queueFrame(core::TimestampNs position);
    [[nodiscard]] quint64 beginCommand(EditorEngineOperation operation,
                                       std::optional<core::TimestampNs> position,
                                       bool countsAsBusy = true,
                                       std::optional<std::int64_t> expectedRevision =
                                           std::nullopt);
    void dispatchNext();
    void postToWorker(QueuedCommand command);
    void handleCompleted(quint64 generation, quint64 commandId, int operation,
                         bool success, const QString& errorMessage);
    void handleFrameCompleted(quint64 generation, quint64 commandId,
                              bool success, const QString& errorMessage,
                              qlonglong revision, qlonglong positionNs,
                              QImage image);
    void requestPlaybackFrame();
    void setPreviewStale(bool value);
    void setPlaying(bool value);
    void setStatus(QString value);

    QThread workerThread_;
    EditorEngineWorker* worker_{};
    MediaBinModel mediaBinModel_;
    TimelineTrackModel timelineTrackModel_;
    std::optional<edit_engine::TimelineSnapshot> snapshot_;
    quint64 generation_{0};
    quint64 nextCommandId_{1};
    int pendingCommands_{0};
    bool previewStale_{false};
    bool playing_{false};
    core::TimestampNs playhead_{};
    core::TimestampNs playbackStart_{};
    QElapsedTimer playbackClock_;
    QTimer playbackTimer_;
    QImage previewImage_;
    QString statusMessage_;
    std::unordered_map<quint64, PendingCommand> commands_;
    std::deque<QueuedCommand> queuedCommands_;
    bool workerCommandActive_{false};
    bool frameRequestInFlight_{false};
};

}  // namespace creator::app
