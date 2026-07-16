#pragma once

#include "app/IRecordingPersistence.h"
#include "capture/IPullCaptureSource.h"
#include "recorder/IRecorder.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

namespace creator::app {

enum class RecordingOperationState { Idle, Preparing, Recording, Finalizing };

/// The only object QML is allowed to talk to.
///
/// QML calls this; this calls the ports. QML never touches a domain object, a
/// capture source or a recorder directly (prompts/01-bootstrap.md 품질기준,
/// CLAUDE.md 7). Everything QML needs is a Q_PROPERTY or a Q_INVOKABLE here.
///
/// The controller holds an IPullCaptureSource and drives it from a QTimer,
/// which is exactly what it does: it pulls. It does not know the source is a
/// fake and must not - reaching through the port to a concrete implementation
/// would break the boundary this whole bootstrap exists to establish.
///
/// R0-03 brings push sources (ScreenCaptureKit, Windows.Graphics.Capture) and
/// this class changes to register a frame callback instead of ticking a timer.
class StudioController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ isBusy NOTIFY operationStateChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(int segmentCount READ segmentCount NOTIFY takeSummaryChanged)
    Q_PROPERTY(QString takeDuration READ takeDuration NOTIFY takeSummaryChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    /// Assembles the fakes itself. This is what main.cpp uses.
    explicit StudioController(QObject* parent = nullptr);

    /// Injects the source and recorder. Tests use this to drive the controller
    /// with no timer and no event loop.
    StudioController(std::unique_ptr<creator::capture::IPullCaptureSource> source,
                     std::unique_ptr<creator::recorder::IRecorder> recorder,
                     QObject* parent = nullptr);
    StudioController(std::unique_ptr<creator::capture::IPullCaptureSource> source,
                     std::unique_ptr<creator::recorder::IRecorder> recorder,
                     IRecordingPersistence* persistence, QObject* parent = nullptr);

    ~StudioController() override;

    [[nodiscard]] bool isBusy() const noexcept {
        return operationState_ == RecordingOperationState::Preparing ||
               operationState_ == RecordingOperationState::Finalizing;
    }
    [[nodiscard]] bool isRecording() const noexcept {
        return operationState_ == RecordingOperationState::Recording;
    }
    [[nodiscard]] int segmentCount() const noexcept { return segmentCount_; }
    [[nodiscard]] QString takeDuration() const { return takeDuration_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }

    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

public slots:
    /// Pulls one frame from the source and hands it to the recorder. Driven by
    /// a QTimer in the app, called directly by tests. A tick while idle is
    /// ignored rather than an error: the timer outlives any single take.
    ///
    /// Safe on the UI thread only because the fake does no I/O. When R0-03
    /// brings real encoding, that work moves off this thread (CLAUDE.md 9).
    void onCaptureTick();

signals:
    void operationStateChanged();
    void recordingChanged();
    void takeSummaryChanged();
    void statusMessageChanged();

private:
    void setOperationState(RecordingOperationState state);
    void handleBeginFinished(core::Result<void> result);
    void abortFailedStart(QString startError);
    void handleCompleteFinished(core::Result<void> result);
    void setStatusMessage(QString message);
    void resetTakeSummary();

    std::unique_ptr<creator::capture::IPullCaptureSource> source_;
    std::unique_ptr<creator::recorder::IRecorder> recorder_;
    IRecordingPersistence* persistence_{};
    QTimer captureTimer_;
    RecordingOperationState operationState_{RecordingOperationState::Idle};
    std::optional<domain::SessionId> pendingSessionId_;
    std::optional<domain::RecordingSession> pendingFinalSession_;
    QString pendingSourceStopError_;
    int segmentCount_{0};
    QString takeDuration_{QStringLiteral("00:00:00")};
    QString statusMessage_;
    std::uint64_t takeCounter_{0};
};

}  // namespace creator::app
