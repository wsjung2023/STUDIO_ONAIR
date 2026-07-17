#pragma once

#include "app/MediaBinModel.h"
#include "app/EditorSessionTypes.h"
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
#include <QUrl>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace creator::app {

class EditorEngineWorker;
class EditorSessionWorker;
enum class EditorEngineOperation;

class EditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* mediaBinModel READ mediaBinModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineTrackModel READ timelineTrackModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool previewStale READ previewStale NOTIFY previewStaleChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qlonglong playheadNs READ playheadNs NOTIFY playheadChanged)
    Q_PROPERTY(qlonglong timelineDurationNs READ timelineDurationNs NOTIFY timelineChanged)
    Q_PROPERTY(qlonglong timelineRevision READ timelineRevision NOTIFY timelineChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QImage previewImage READ previewImage NOTIFY previewImageChanged)
    Q_PROPERTY(bool hasPreviewFrame READ hasPreviewFrame NOTIFY previewImageChanged)
    Q_PROPERTY(bool sessionBusy READ sessionBusy NOTIFY sessionBusyChanged)
    Q_PROPERTY(QString selectedTrackId READ selectedTrackId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY selectionChanged)
    Q_PROPERTY(qlonglong rangeInNs READ rangeInNs NOTIFY markedRangeChanged)
    Q_PROPERTY(qlonglong rangeOutNs READ rangeOutNs NOTIFY markedRangeChanged)
    Q_PROPERTY(bool hasMarkedRange READ hasMarkedRange NOTIFY markedRangeChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editStateChanged)
    Q_PROPERTY(bool clean READ clean NOTIFY editStateChanged)

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
    [[nodiscard]] qlonglong timelineDurationNs() const noexcept;
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] QImage previewImage() const { return previewImage_; }
    [[nodiscard]] bool hasPreviewFrame() const noexcept {
        return !previewImage_.isNull();
    }
    [[nodiscard]] bool sessionBusy() const noexcept { return sessionBusy_; }
    [[nodiscard]] QString selectedTrackId() const { return selectedTrackId_; }
    [[nodiscard]] QString selectedClipId() const { return selectedClipId_; }
    [[nodiscard]] qlonglong rangeInNs() const noexcept {
        return rangeIn_.has_value()
                   ? rangeIn_->time_since_epoch().count()
                   : -1;
    }
    [[nodiscard]] qlonglong rangeOutNs() const noexcept {
        return rangeOut_.has_value()
                   ? rangeOut_->time_since_epoch().count()
                   : -1;
    }
    [[nodiscard]] bool hasMarkedRange() const noexcept {
        return rangeIn_.has_value() && rangeOut_.has_value() &&
               *rangeIn_ < *rangeOut_;
    }
    [[nodiscard]] bool canUndo() const noexcept { return canUndo_; }
    [[nodiscard]] bool canRedo() const noexcept { return canRedo_; }
    [[nodiscard]] bool clean() const noexcept { return clean_; }

    void openSession(std::vector<domain::MediaAsset> assets,
                     edit_engine::TimelineSnapshot snapshot);
    void commitTimeline(edit_engine::TimelineChangeSet change);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(qlonglong positionNs);
    Q_INVOKABLE void selectClip(QString trackId, QString clipId);
    Q_INVOKABLE void markRangeIn();
    Q_INVOKABLE void markRangeOut();
    Q_INVOKABLE void openProject(QUrl projectUrl);
    Q_INVOKABLE void splitSelected();
    Q_INVOKABLE void trimSelectedStart();
    Q_INVOKABLE void trimSelectedEnd();
    Q_INVOKABLE void deleteMarkedRange(bool ripple);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void save();

signals:
    void busyChanged();
    void previewStaleChanged();
    void playingChanged();
    void playheadChanged();
    void timelineChanged();
    void statusMessageChanged();
    void previewImageChanged();
    void sessionBusyChanged();
    void selectionChanged();
    void markedRangeChanged();
    void editStateChanged();

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
    void handleSessionOpened(quint64 generation,
                             EditorSessionResultPtr result);
    void handleSessionEdited(quint64 generation, quint64 commandId,
                             EditorSessionResultPtr result);
    void queueSessionEdit(EditorEditRequest request);
    void publishSessionState(const EditorSessionState& state);
    void setSessionBusy(bool value);
    void requestPlaybackFrame();
    void setPreviewStale(bool value);
    void setPlaying(bool value);
    void setStatus(QString value);

    QThread workerThread_;
    EditorEngineWorker* worker_{};
    QThread sessionThread_;
    EditorSessionWorker* sessionWorker_{};
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
    bool sessionBusy_{false};
    QString selectedTrackId_;
    QString selectedClipId_;
    std::optional<core::TimestampNs> rangeIn_;
    std::optional<core::TimestampNs> rangeOut_;
    bool canUndo_{false};
    bool canRedo_{false};
    bool clean_{true};
    bool durableSessionReady_{false};
    quint64 sessionGeneration_{0};
    quint64 nextSessionCommandId_{1};
    std::optional<quint64> activeSessionCommand_;
};

}  // namespace creator::app
