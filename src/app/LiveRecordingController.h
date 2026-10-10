#pragma once

#include "app/ILiveRecordingEngine.h"
#include "app/IRecordingPersistence.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace creator::app {

enum class LiveRecordingOperationState { Idle, Preparing, Recording, Finalizing };

/// QML-facing owner of the production live recording lifecycle.
class LiveRecordingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ isBusy NOTIFY operationStateChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(bool recordingAvailable READ recordingAvailable CONSTANT)
    Q_PROPERTY(int segmentCount READ segmentCount NOTIFY diagnosticsChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY diagnosticsChanged)
    Q_PROPERTY(qulonglong queuedItems READ queuedItems NOTIFY diagnosticsChanged)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames NOTIFY diagnosticsChanged)
    Q_PROPERTY(qulonglong syncDroppedFrames READ syncDroppedFrames NOTIFY diagnosticsChanged)
    Q_PROPERTY(qulonglong duplicatedFrames READ duplicatedFrames NOTIFY diagnosticsChanged)
    Q_PROPERTY(double maximumDriftMilliseconds READ maximumDriftMilliseconds NOTIFY diagnosticsChanged)
    Q_PROPERTY(double audioCorrectionPpm READ audioCorrectionPpm NOTIFY diagnosticsChanged)
    Q_PROPERTY(qulonglong diskAvailableBytes READ diskAvailableBytes NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString encoderName READ encoderName NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString takeDuration READ takeDuration NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionIdString NOTIFY activeRecordingChanged)
    Q_PROPERTY(qlonglong recordingStartedAtNs READ recordingStartedAtNs NOTIFY activeRecordingChanged)
    Q_PROPERTY(qlonglong recordingPositionNs READ recordingPositionNs NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    /// Why the last take failed (start refused, capture died, persistence
    /// failed). Empty while nothing has gone wrong and cleared when a new take
    /// starts, so the UI can keep the reason on screen until the creator acts.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    using PackagePathProvider =
        std::function<std::optional<std::filesystem::path>()>;
    using Clock = std::function<core::TimestampNs()>;
    using PreparationCompletion = std::function<void(core::Result<void>)>;
    using RecordingPreparation =
        std::function<void(const LiveRecordingStart&, PreparationCompletion)>;

    LiveRecordingController(std::unique_ptr<ILiveRecordingEngine> engine,
                            IRecordingPersistence* persistence,
                            PackagePathProvider packagePathProvider,
                            Clock clock = [] { return core::ProjectClock::now(); },
                            QObject* parent = nullptr);
    ~LiveRecordingController() override;

    [[nodiscard]] LiveRecordingOperationState operationState() const noexcept {
        return operationState_;
    }
    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] bool isRecording() const noexcept;
    [[nodiscard]] bool recordingAvailable() const noexcept;
    [[nodiscard]] int segmentCount() const noexcept { return segmentCount_; }
    [[nodiscard]] int trackCount() const noexcept { return trackCount_; }
    [[nodiscard]] qulonglong queuedItems() const noexcept { return queuedItems_; }
    [[nodiscard]] qulonglong droppedFrames() const noexcept { return droppedFrames_; }
    [[nodiscard]] qulonglong syncDroppedFrames() const noexcept {
        return syncDroppedFrames_;
    }
    [[nodiscard]] qulonglong duplicatedFrames() const noexcept {
        return duplicatedFrames_;
    }
    [[nodiscard]] double maximumDriftMilliseconds() const noexcept {
        return maximumDriftMilliseconds_;
    }
    [[nodiscard]] double audioCorrectionPpm() const noexcept {
        return audioCorrectionPpm_;
    }
    [[nodiscard]] qulonglong diskAvailableBytes() const noexcept {
        return diskAvailableBytes_;
    }
    [[nodiscard]] QString encoderName() const { return encoderName_; }
    [[nodiscard]] QString takeDuration() const { return takeDuration_; }
    [[nodiscard]] std::optional<domain::SessionId> activeSessionId() const;
    [[nodiscard]] std::optional<core::TimestampNs> recordingStartedAt() const;
    [[nodiscard]] std::optional<core::TimestampNs> recordingPosition() const;
    [[nodiscard]] QString activeSessionIdString() const;
    [[nodiscard]] qlonglong recordingStartedAtNs() const noexcept;
    [[nodiscard]] qlonglong recordingPositionNs() const noexcept;
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] QString lastError() const { return lastError_; }

    void setRecordingPreparation(RecordingPreparation preparation);

    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

public slots:
    void pollDiagnostics();

signals:
    void operationStateChanged();
    void recordingChanged();
    void diagnosticsChanged();
    void activeRecordingChanged();
    void recordingCommitted(QString sessionId);
    void recordingAborted(QString sessionId);
    void statusMessageChanged();
    void lastErrorChanged();

private:
    void handleBeginFinished(quint64 generation, domain::SessionId sessionId,
                             core::Result<void> result);
    void handlePreparationFinished(quint64 generation,
                                   domain::SessionId sessionId,
                                   core::Result<void> result);
    void startEngine();
    void handleEngineFinished(quint64 generation, domain::SessionId sessionId,
                              core::Result<LiveRecordingCompletion> result);
    void handlePersistenceComplete(quint64 generation,
                                   domain::SessionId sessionId,
                                   core::Result<void> result);
    void abortStartedSession(core::AppError error);
    void setOperationState(LiveRecordingOperationState state);
    void setStatusMessage(QString message);
    /// Records a take failure: shown via lastError AND written to the warning
    /// log, so a dead recording is never silent (CLAUDE.md rule 9).
    void reportFailure(const core::AppError& error);
    void clearLastError();
    void resetDiagnostics();

    std::unique_ptr<ILiveRecordingEngine> engine_;
    IRecordingPersistence* persistence_{};
    PackagePathProvider packagePathProvider_;
    Clock clock_;
    RecordingPreparation recordingPreparation_;
    QTimer diagnosticsTimer_;
    LiveRecordingOperationState operationState_{LiveRecordingOperationState::Idle};
    std::optional<LiveRecordingStart> pendingStart_;
    std::optional<LiveRecordingCompletion> pendingCompletion_;
    std::optional<core::AppError> pendingTerminalError_;
    quint64 generation_{0};
    int segmentCount_{0};
    int trackCount_{0};
    qulonglong queuedItems_{0};
    qulonglong droppedFrames_{0};
    qulonglong syncDroppedFrames_{0};
    qulonglong duplicatedFrames_{0};
    double maximumDriftMilliseconds_{0.0};
    double audioCorrectionPpm_{0.0};
    qulonglong diskAvailableBytes_{0};
    QString encoderName_{QStringLiteral("Not active")};
    QString takeDuration_{QStringLiteral("00:00:00")};
    QString statusMessage_;
    QString lastError_;
};

}  // namespace creator::app
