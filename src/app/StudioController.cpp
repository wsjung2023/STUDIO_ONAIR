#include "app/StudioController.h"

#include "core/Timebase.h"
#include "core/Uuid.h"
#include "domain/Identifiers.h"
#include "recorder/IRecorder.h"

#include <chrono>
#include <QPointer>
#include <utility>

namespace creator::app {
namespace {

using core::DurationNs;

/// 60fps. The fake computes timestamps from frame indices, so this interval
/// only sets how fast the preview updates - it cannot make the take's recorded
/// duration wrong.
constexpr int kCaptureIntervalMs = 16;

/// The capture geometry and frame rate this take is recorded at. Named so
/// startRecording() (which starts the source with it) and stopRecording()
/// (which needs the same rate back to convert a frame count into a timestamp)
/// cannot drift apart into two different literals.
constexpr capture::CaptureConfig kCaptureConfig{};

/// Every take's timeline starts here, on the same footing as the source's own
/// frame 0. See the long comment in startRecording() for why this must not be
/// core::ProjectClock::now().
constexpr core::TimestampNs kSessionStart{};

QString formatDuration(DurationNs duration) {
    const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

}  // namespace

StudioController::StudioController(std::unique_ptr<capture::IPullCaptureSource> source,
                                   std::unique_ptr<recorder::IRecorder> recorder, QObject* parent)
    : StudioController(std::move(source), std::move(recorder), nullptr, parent) {}

StudioController::StudioController(std::unique_ptr<capture::IPullCaptureSource> source,
                                   std::unique_ptr<recorder::IRecorder> recorder,
                                   IRecordingPersistence* persistence, QObject* parent)
    : QObject(parent),
      source_(std::move(source)),
      recorder_(std::move(recorder)),
      persistence_(persistence) {
    captureTimer_.setInterval(kCaptureIntervalMs);
    connect(&captureTimer_, &QTimer::timeout, this, &StudioController::onCaptureTick);
}

StudioController::~StudioController() {
    // The take is lost either way at this point, but the source must still let
    // go of its OS handles (CLAUDE.md 8 requires resource cleanup). The
    // returned session is discarded, so the exact stop instant does not
    // matter here - it only has to be >= kSessionStart, the same value
    // startRecording() opened this take with, or RecordingSession::stop
    // rejects it as preceding the start time.
    //
    // Both Results are discarded here, error included, on purpose: this
    // object is being destroyed, so there is no statusMessage() a QML binding
    // will ever read again to show one in. That is different from
    // stopRecording() and the startRecording() unwind below, where the same
    // calls' Results are routed to setStatusMessage() because the UI is still
    // there to see it.
    if (isRecording()) {
        static_cast<void>(recorder_->stop(kSessionStart));
        static_cast<void>(source_->stop());
    }
}

void StudioController::startRecording() {
    if (operationState_ != RecordingOperationState::Idle) {
        return;
    }

    auto sessionId = domain::SessionId::create(core::generateUuidV4());
    if (!sessionId.hasValue()) {
        setStatusMessage(QString::fromStdString(sessionId.error().message()));
        return;
    }

    pendingSessionId_ = std::move(sessionId).value();
    setOperationState(RecordingOperationState::Preparing);
    setStatusMessage(tr("Preparing recording"));
    QPointer<StudioController> self{this};
    auto completion = [self](core::Result<void> result) mutable {
        if (self) self->handleBeginFinished(std::move(result));
    };
    if (persistence_) {
        persistence_->begin(*pendingSessionId_, kSessionStart, std::move(completion));
    } else {
        completion(core::ok());
    }
}

void StudioController::handleBeginFinished(core::Result<void> result) {
    if (operationState_ != RecordingOperationState::Preparing || !pendingSessionId_) return;
    if (!result.hasValue()) {
        pendingSessionId_.reset();
        setOperationState(RecordingOperationState::Idle);
        setStatusMessage(QString::fromStdString(result.error().message()));
        return;
    }

    const recorder::RecorderConfig config{
        .sessionId = *pendingSessionId_,
        .sourceId = source_->id(),
        .segmentDuration = std::chrono::seconds{2},
    };
    // The recorder's segment boundaries (FakeRecorder::accept's
    // frame.timestamp - segmentStart_ comparison) and the session's own
    // duration are both computed purely from the TimestampNs values this
    // class hands to start()/stop(). FakeCaptureSource restarts its own frame
    // timeline at zero on every start() - frame 0 is always timestamp zero,
    // never core::ProjectClock::now() - so starting the recorder's session at
    // a fresh "now" reading would put segmentStart_ on a completely different
    // scale from the frame timestamps accept() is about to receive: no
    // segment would ever close mid-take, and duration() would measure real
    // wall-clock time (microseconds for a tight test loop) instead of
    // simulated recording time. CLAUDE.md 9 bans wall-clock-based A/V sync for
    // the same underlying reason - an independently-read clock has no
    // relationship to the media's own timeline. Anchoring at kSessionStart
    // (zero) keeps both timelines on the same footing.
    if (auto started = recorder_->start(config, kSessionStart); !started.hasValue()) {
        abortFailedStart(QString::fromStdString(started.error().message()));
        return;
    }
    if (auto started = source_->start(kCaptureConfig); !started.hasValue()) {
        // Unwind the recorder so a failed source does not leave a take half
        // open. The source itself must be unwound too: ICaptureSource::stop()
        // is documented safe to call on a source that was never started -
        // precisely so this unconditional call is safe here - and a real
        // SCK/WGC source that acquires a stream and then fails a later
        // permission check would otherwise leak it.
        static_cast<void>(recorder_->stop(kSessionStart));
        if (auto stopped = source_->stop(); !stopped.hasValue()) {
            // Releasing the source failed too. That is now the more
            // actionable problem (CLAUDE.md 9 requires device errors reach
            // the user), so it takes the status message instead of the
            // start failure that triggered this unwind.
            abortFailedStart(QString::fromStdString(stopped.error().message()));
        } else {
            abortFailedStart(QString::fromStdString(started.error().message()));
        }
        return;
    }

    resetTakeSummary();
    pendingSessionId_.reset();
    setOperationState(RecordingOperationState::Recording);
    captureTimer_.start();
    setStatusMessage(tr("Recording"));
}

void StudioController::abortFailedStart(QString startError) {
    if (!pendingSessionId_) {
        setOperationState(RecordingOperationState::Idle);
        setStatusMessage(std::move(startError));
        return;
    }
    if (!persistence_) {
        pendingSessionId_.reset();
        setOperationState(RecordingOperationState::Idle);
        setStatusMessage(std::move(startError));
        return;
    }
    QPointer<StudioController> self{this};
    persistence_->abort(
        *pendingSessionId_, startError.toStdString(),
        [self, startError = std::move(startError)](core::Result<void> result) mutable {
            if (!self) return;
            self->pendingSessionId_.reset();
            self->setOperationState(RecordingOperationState::Idle);
            self->setStatusMessage(result.hasValue()
                                       ? std::move(startError)
                                       : QString::fromStdString(result.error().message()));
        });
}

void StudioController::stopRecording() {
    if (!isRecording()) {
        if (operationState_ == RecordingOperationState::Idle) {
            setStatusMessage(tr("Not recording"));
        }
        return;
    }

    captureTimer_.stop();
    setOperationState(RecordingOperationState::Finalizing);
    setStatusMessage(tr("Finalizing recording"));

    // Stop on the same frame-timestamp scale the take started on (see
    // startRecording()). stats().receivedFrames is the count of frames the
    // source actually delivered this take - a plain counter the
    // ICaptureSource port already exposes, not anything fake-specific -
    // and running it back through frameToTimestamp gives the instant the
    // *next* frame would have landed on, i.e. exactly where this take
    // stopped, using the same ceiling rule frameToTimestamp itself defines
    // rather than approximating it.
    core::TimestampNs stopAt = kSessionStart;
    if (auto rate = core::FrameRate::create(
            static_cast<std::int64_t>(kCaptureConfig.frameRateNumerator),
            static_cast<std::int64_t>(kCaptureConfig.frameRateDenominator));
        rate.hasValue()) {
        const auto framesCaptured = static_cast<std::int64_t>(source_->stats().receivedFrames);
        stopAt = core::frameToTimestamp(framesCaptured, rate.value());
    }

    auto session = recorder_->stop(stopAt);
    auto sourceStopped = source_->stop();

    if (!session.hasValue()) {
        setOperationState(RecordingOperationState::Idle);
        setStatusMessage(QString::fromStdString(session.error().message()));
        return;
    }

    pendingFinalSession_ = std::move(session).value();
    pendingSourceStopError_ = sourceStopped.hasValue()
                                  ? QString{}
                                  : QString::fromStdString(sourceStopped.error().message());
    QPointer<StudioController> self{this};
    auto completion = [self](core::Result<void> result) mutable {
        if (self) self->handleCompleteFinished(std::move(result));
    };
    if (persistence_) {
        persistence_->complete(*pendingFinalSession_, std::move(completion));
    } else {
        completion(core::ok());
    }
}

void StudioController::handleCompleteFinished(core::Result<void> result) {
    if (operationState_ != RecordingOperationState::Finalizing || !pendingFinalSession_) return;
    if (!result.hasValue()) {
        pendingFinalSession_.reset();
        pendingSourceStopError_.clear();
        setOperationState(RecordingOperationState::Idle);
        setStatusMessage(QString::fromStdString(result.error().message()));
        return;
    }

    segmentCount_ = static_cast<int>(pendingFinalSession_->segmentCount());
    // Always engaged here: the session just stopped successfully. Handling the
    // nullopt branch anyway rather than value_or-ing a zero, because a zero
    // would be indistinguishable from a real zero-length take on screen.
    if (const auto takeLength = pendingFinalSession_->duration()) {
        takeDuration_ = formatDuration(*takeLength);
    }
    emit takeSummaryChanged();

    const QString finalStatus = pendingSourceStopError_.isEmpty()
                                    ? tr("Stopped")
                                    : pendingSourceStopError_;
    pendingFinalSession_.reset();
    pendingSourceStopError_.clear();
    setOperationState(RecordingOperationState::Idle);
    if (finalStatus != tr("Stopped")) {
        // The take itself is already safely finalised above; failing to
        // release the source is a device-handle problem (CLAUDE.md 9: device
        // errors must reach the user), not a recording-content problem, but it
        // must still not be swallowed the way a bare static_cast<void> would.
        setStatusMessage(finalStatus);
    } else {
        setStatusMessage(tr("Stopped"));
    }
}

void StudioController::onCaptureTick() {
    if (!isRecording()) {
        return;
    }

    auto frame = source_->tick();
    if (!frame.hasValue()) {
        setStatusMessage(QString::fromStdString(frame.error().message()));
        return;
    }
    if (auto accepted = recorder_->accept(frame.value()); !accepted.hasValue()) {
        setStatusMessage(QString::fromStdString(accepted.error().message()));
    }
}

void StudioController::setOperationState(RecordingOperationState state) {
    if (operationState_ == state) return;
    const bool wasRecording = isRecording();
    operationState_ = state;
    emit operationStateChanged();
    if (wasRecording != isRecording()) emit recordingChanged();
}

void StudioController::setStatusMessage(QString message) {
    if (statusMessage_ == message) {
        return;
    }
    statusMessage_ = std::move(message);
    emit statusMessageChanged();
}

void StudioController::resetTakeSummary() {
    segmentCount_ = 0;
    takeDuration_ = QStringLiteral("00:00:00");
    emit takeSummaryChanged();
}

}  // namespace creator::app
