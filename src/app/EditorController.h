#pragma once

#include "app/MediaBinModel.h"
#include "app/EditorSessionTypes.h"
#include "app/TimelineTrackModel.h"
#include "core/Timebase.h"
#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/IEditEngine.h"
#include "transcription/Transcript.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

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
    Q_PROPERTY(QString selectedClipKind READ selectedClipKind NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedVisualCompatible READ selectedVisualCompatible NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedAudioCompatible READ selectedAudioCompatible NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedVisualTransform READ selectedVisualTransform NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedAudioEnvelope READ selectedAudioEnvelope NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedTitlePayload READ selectedTitlePayload NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList selectedCaptionCues READ selectedCaptionCues NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList transcriptSegments READ transcriptSegments NOTIFY transcriptChanged)
    Q_PROPERTY(QString selectedPipPreset READ selectedPipPreset NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedResolvedFontFamily READ selectedResolvedFontFamily NOTIFY selectionChanged)
    Q_PROPERTY(qlonglong selectedClipStartNs READ selectedClipStartNs NOTIFY selectionChanged)
    Q_PROPERTY(qlonglong selectedClipEndNs READ selectedClipEndNs NOTIFY selectionChanged)
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
    [[nodiscard]] QString selectedClipKind() const;
    [[nodiscard]] bool selectedVisualCompatible() const noexcept;
    [[nodiscard]] bool selectedAudioCompatible() const noexcept;
    [[nodiscard]] QVariantMap selectedVisualTransform() const;
    [[nodiscard]] QVariantMap selectedAudioEnvelope() const;
    [[nodiscard]] QVariantMap selectedTitlePayload() const;
    [[nodiscard]] QVariantList selectedCaptionCues() const;
    [[nodiscard]] QVariantList transcriptSegments() const { return transcriptSegments_; }
    [[nodiscard]] QString selectedPipPreset() const;
    [[nodiscard]] QString selectedResolvedFontFamily() const;
    [[nodiscard]] qlonglong selectedClipStartNs() const noexcept;
    [[nodiscard]] qlonglong selectedClipEndNs() const noexcept;
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
    [[nodiscard]] std::optional<edit_engine::TimelineSnapshot>
    exportSnapshot() const { return snapshot_; }
    [[nodiscard]] core::Result<void> approveTranscriptProposal(
        const transcription::Transcript& transcript,
        std::int64_t expectedRevision);
    [[nodiscard]] core::Result<void> approveCutProposal(
        const domain::TimeRange& range, std::int64_t expectedRevision,
        bool ripple = true);

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
    Q_INVOKABLE void applySelectedVisualTransform(
        double x, double y, double width, double height, double scaleX,
        double scaleY, double rotationDegrees, double cropLeft, double cropTop,
        double cropRight, double cropBottom, double opacity, int zOrder);
    Q_INVOKABLE void applySelectedPipPreset(QString preset);
    Q_INVOKABLE void resetSelectedVisualTransform();
    Q_INVOKABLE void applySelectedAudioEnvelope(
        double gainDb, qlonglong fadeInNs, qlonglong fadeOutNs);
    Q_INVOKABLE void resetSelectedAudioEnvelope();
    Q_INVOKABLE void addTitle(
        QString text, QString fontFamily, double x, double y,
        QString foreground, QString background, QString alignment);
    Q_INVOKABLE void editSelectedTitle(
        QString text, QString fontFamily, double x, double y,
        QString foreground, QString background, QString alignment);
    Q_INVOKABLE void removeSelectedTitle();
    Q_INVOKABLE void addCaptionCue(
        qlonglong startOffsetNs, qlonglong durationNs, QString text);
    Q_INVOKABLE void editCaptionCue(
        QString cueId, qlonglong startOffsetNs, qlonglong durationNs,
        QString text);
    Q_INVOKABLE void removeCaptionCue(QString cueId);
    Q_INVOKABLE bool loadTranscript(QUrl transcriptUrl);
    Q_INVOKABLE void addTranscriptSegment(int index);
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
    void transcriptChanged();
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
    void publishTranscript(const transcription::Transcript& transcript,
                           QString status);
    [[nodiscard]] const domain::Clip* selectedClip() const noexcept;
    [[nodiscard]] const domain::Track* selectedTrack() const noexcept;

    QThread workerThread_;
    EditorEngineWorker* worker_{};
    QThread sessionThread_;
    EditorSessionWorker* sessionWorker_{};
    MediaBinModel mediaBinModel_;
    TimelineTrackModel timelineTrackModel_;
    std::optional<edit_engine::TimelineSnapshot> snapshot_;
    QVariantList transcriptSegments_;
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
    bool derivedFailureRecoveryPending_{false};
    quint64 sessionGeneration_{0};
    quint64 nextSessionCommandId_{1};
    std::optional<quint64> activeSessionCommand_;
};

}  // namespace creator::app
