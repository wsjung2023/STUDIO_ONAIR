#include "app/LiveRecordingController.h"

#include "core/Uuid.h"

#include <QPointer>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace creator::app {
namespace {

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString formatDuration(core::DurationNs duration) {
    const auto totalSeconds = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::seconds>(duration).count());
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

int boundedInt(std::uint64_t value) noexcept {
    return static_cast<int>(std::min<std::uint64_t>(
        value, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

}  // namespace

LiveRecordingController::LiveRecordingController(
    std::unique_ptr<ILiveRecordingEngine> engine, IRecordingPersistence* persistence,
    PackagePathProvider packagePathProvider, Clock clock, QObject* parent)
    : QObject(parent),
      engine_(std::move(engine)),
      persistence_(persistence),
      packagePathProvider_(std::move(packagePathProvider)),
      clock_(std::move(clock)) {
    diagnosticsTimer_.setInterval(250);
    connect(&diagnosticsTimer_, &QTimer::timeout,
            this, &LiveRecordingController::pollDiagnostics);
}

LiveRecordingController::~LiveRecordingController() {
    ++generation_;
    diagnosticsTimer_.stop();
    if (engine_ && (isRecording() ||
                    operationState_ == LiveRecordingOperationState::Finalizing)) {
        engine_->stopAsync(clock_());
    }
}

bool LiveRecordingController::isBusy() const noexcept {
    return operationState_ == LiveRecordingOperationState::Preparing ||
           operationState_ == LiveRecordingOperationState::Finalizing;
}

bool LiveRecordingController::isRecording() const noexcept {
    return operationState_ == LiveRecordingOperationState::Recording;
}

bool LiveRecordingController::recordingAvailable() const noexcept {
    return engine_ && engine_->available();
}

std::optional<domain::SessionId>
LiveRecordingController::activeSessionId() const {
    return pendingStart_.has_value()
               ? std::optional<domain::SessionId>{pendingStart_->sessionId}
               : std::nullopt;
}

std::optional<core::TimestampNs>
LiveRecordingController::recordingStartedAt() const {
    return pendingStart_.has_value()
               ? std::optional<core::TimestampNs>{pendingStart_->startedAt}
               : std::nullopt;
}

std::optional<core::TimestampNs>
LiveRecordingController::recordingPosition() const {
    if (!pendingStart_) return std::nullopt;
    return core::TimestampNs{
        std::max(core::DurationNs::zero(), clock_() - pendingStart_->startedAt)};
}

QString LiveRecordingController::activeSessionIdString() const {
    const auto session = activeSessionId();
    return session.has_value() ? fromUtf8(session->value()) : QString{};
}

qlonglong LiveRecordingController::recordingStartedAtNs() const noexcept {
    return pendingStart_.has_value()
               ? pendingStart_->startedAt.time_since_epoch().count()
               : 0;
}

qlonglong LiveRecordingController::recordingPositionNs() const noexcept {
    const auto position = recordingPosition();
    return position.has_value() ? position->time_since_epoch().count() : 0;
}

void LiveRecordingController::setRecordingPreparation(
    RecordingPreparation preparation) {
    if (preparation && operationState_ != LiveRecordingOperationState::Idle) return;
    recordingPreparation_ = std::move(preparation);
}

void LiveRecordingController::startRecording() {
    if (operationState_ != LiveRecordingOperationState::Idle) return;
    if (!engine_ || !engine_->available()) {
        setStatusMessage(engine_ ? fromUtf8(engine_->unavailableReason())
                                 : tr("Live recording engine is unavailable"));
        return;
    }
    if (!persistence_ || !packagePathProvider_) {
        setStatusMessage(tr("Recording persistence is unavailable"));
        return;
    }
    auto packagePath = packagePathProvider_();
    if (!packagePath) {
        setStatusMessage(tr("Open a local project before recording"));
        return;
    }
    auto sources = engine_->sourceSnapshot();
    if (!sources.hasValue()) {
        setStatusMessage(fromUtf8(sources.error().message()));
        return;
    }
    auto sessionId = domain::SessionId::create(core::generateUuidV4());
    if (!sessionId.hasValue()) {
        setStatusMessage(fromUtf8(sessionId.error().message()));
        return;
    }

    ++generation_;
    resetDiagnostics();
    pendingStart_ = LiveRecordingStart{.sessionId = std::move(sessionId).value(),
                                       .packagePath = std::move(*packagePath),
                                       .startedAt = clock_(),
                                       .sources = std::move(sources).value()};
    emit activeRecordingChanged();
    setOperationState(LiveRecordingOperationState::Preparing);
    setStatusMessage(tr("Preparing recording"));
    QPointer<LiveRecordingController> self{this};
    const auto generation = generation_;
    const auto activeSessionId = pendingStart_->sessionId;
    persistence_->begin(
        pendingStart_->sessionId, pendingStart_->startedAt,
        [self, generation,
         activeSessionId](core::Result<void> result) mutable {
            if (!self) return;
            auto dispatch = [self, generation, activeSessionId,
                             result = std::move(result)]() mutable {
                if (self) {
                    self->handleBeginFinished(generation, activeSessionId,
                                              std::move(result));
                }
            };
            if (self->thread() == QThread::currentThread()) {
                dispatch();
            } else {
                QMetaObject::invokeMethod(self, std::move(dispatch),
                                          Qt::QueuedConnection);
            }
        });
}

void LiveRecordingController::handleBeginFinished(
    quint64 generation, domain::SessionId sessionId,
    core::Result<void> result) {
    if (generation != generation_ ||
        operationState_ != LiveRecordingOperationState::Preparing ||
        !pendingStart_ || pendingStart_->sessionId != sessionId) return;
    if (!result.hasValue()) {
        pendingStart_.reset();
        emit activeRecordingChanged();
        setOperationState(LiveRecordingOperationState::Idle);
        setStatusMessage(fromUtf8(result.error().message()));
        return;
    }

    if (recordingPreparation_) {
        const auto activeGeneration = generation_;
        const auto preparedSessionId = pendingStart_->sessionId;
        QPointer<LiveRecordingController> self{this};
        recordingPreparation_(
            *pendingStart_,
            [self, activeGeneration, preparedSessionId](
                core::Result<void> prepared) mutable {
                if (!self) return;
                auto dispatch =
                    [self, activeGeneration, preparedSessionId,
                     prepared = std::move(prepared)]() mutable {
                        if (self) {
                            self->handlePreparationFinished(
                                activeGeneration, preparedSessionId,
                                std::move(prepared));
                        }
                    };
                if (self->thread() == QThread::currentThread()) {
                    dispatch();
                } else {
                    QMetaObject::invokeMethod(self, std::move(dispatch),
                                              Qt::QueuedConnection);
                }
            });
        return;
    }
    startEngine();
}

void LiveRecordingController::handlePreparationFinished(
    quint64 generation, domain::SessionId sessionId,
    core::Result<void> result) {
    if (generation != generation_ ||
        operationState_ != LiveRecordingOperationState::Preparing ||
        !pendingStart_ || pendingStart_->sessionId != sessionId) {
        return;
    }
    if (!result.hasValue()) {
        abortStartedSession(result.error());
        return;
    }
    startEngine();
}

void LiveRecordingController::startEngine() {
    if (operationState_ != LiveRecordingOperationState::Preparing ||
        !pendingStart_) {
        return;
    }

    QPointer<LiveRecordingController> self{this};
    const auto generation = generation_;
    const auto sessionId = pendingStart_->sessionId;
    auto completion = [self, generation, sessionId](
                          core::Result<LiveRecordingCompletion> completed) mutable {
        if (!self) return;
        if (self->thread() == QThread::currentThread()) {
            self->handleEngineFinished(generation, sessionId,
                                       std::move(completed));
            return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, generation, sessionId,
             completed = std::move(completed)]() mutable {
                if (self) {
                    self->handleEngineFinished(generation, sessionId,
                                               std::move(completed));
                }
            },
            Qt::QueuedConnection);
    };
    auto started = engine_->start(*pendingStart_, std::move(completion));
    if (!started.hasValue()) {
        abortStartedSession(started.error());
        return;
    }
    setOperationState(LiveRecordingOperationState::Recording);
    diagnosticsTimer_.start();
    pollDiagnostics();
    setStatusMessage(tr("Recording"));
}

void LiveRecordingController::abortStartedSession(core::AppError error) {
    if (!pendingStart_ || !persistence_) {
        pendingStart_.reset();
        setOperationState(LiveRecordingOperationState::Idle);
        setStatusMessage(fromUtf8(error.message()));
        return;
    }
    const auto sessionId = pendingStart_->sessionId;
    const auto generation = generation_;
    const auto reason = error.message();
    QPointer<LiveRecordingController> self{this};
    persistence_->abort(
        sessionId, reason,
        [self, generation, sessionId,
         error = std::move(error)](core::Result<void> aborted) mutable {
            if (!self) return;
            auto dispatch = [self, generation, sessionId,
                             error = std::move(error),
                             aborted = std::move(aborted)]() mutable {
                if (!self || generation != self->generation_ ||
                    !self->pendingStart_ ||
                    self->pendingStart_->sessionId != sessionId) {
                    return;
                }
                self->pendingStart_.reset();
                emit self->activeRecordingChanged();
                self->setOperationState(LiveRecordingOperationState::Idle);
                self->setStatusMessage(fromUtf8(
                    aborted.hasValue() ? error.message()
                                       : aborted.error().message()));
                if (aborted.hasValue()) {
                    emit self->recordingAborted(fromUtf8(sessionId.value()));
                }
            };
            if (self->thread() == QThread::currentThread()) {
                dispatch();
            } else {
                QMetaObject::invokeMethod(self, std::move(dispatch),
                                          Qt::QueuedConnection);
            }
        });
}

void LiveRecordingController::stopRecording() {
    if (!isRecording()) {
        if (operationState_ == LiveRecordingOperationState::Idle) {
            setStatusMessage(tr("Not recording"));
        }
        return;
    }
    diagnosticsTimer_.stop();
    pollDiagnostics();
    setOperationState(LiveRecordingOperationState::Finalizing);
    setStatusMessage(tr("Finalizing recording"));
    engine_->stopAsync(clock_());
}

void LiveRecordingController::handleEngineFinished(
    quint64 generation, domain::SessionId sessionId,
    core::Result<LiveRecordingCompletion> result) {
    if (generation != generation_ || !pendingStart_ ||
        pendingStart_->sessionId != sessionId) return;
    if (operationState_ != LiveRecordingOperationState::Recording &&
        operationState_ != LiveRecordingOperationState::Finalizing) {
        return;
    }
    diagnosticsTimer_.stop();
    pollDiagnostics();
    setOperationState(LiveRecordingOperationState::Finalizing);
    if (!result.hasValue()) {
        abortStartedSession(result.error());
        return;
    }
    pendingCompletion_ = std::move(result).value();
    pendingTerminalError_ = pendingCompletion_->terminalError;
    segmentCount_ = boundedInt(pendingCompletion_->segmentsPublished);
    trackCount_ = boundedInt(pendingCompletion_->trackCount);
    droppedFrames_ = pendingCompletion_->videoFramesDropped;
    if (const auto duration = pendingCompletion_->session.duration()) {
        takeDuration_ = formatDuration(*duration);
    }
    emit diagnosticsChanged();

    QPointer<LiveRecordingController> self{this};
    persistence_->complete(
        pendingCompletion_->session,
        [self, generation, sessionId](core::Result<void> completed) mutable {
            if (!self) return;
            auto dispatch = [self, generation, sessionId,
                             completed = std::move(completed)]() mutable {
                if (self) {
                    self->handlePersistenceComplete(generation, sessionId,
                                                    std::move(completed));
                }
            };
            if (self->thread() == QThread::currentThread()) {
                dispatch();
            } else {
                QMetaObject::invokeMethod(self, std::move(dispatch),
                                          Qt::QueuedConnection);
            }
        });
}

void LiveRecordingController::handlePersistenceComplete(
    quint64 generation, domain::SessionId sessionId,
    core::Result<void> result) {
    if (generation != generation_ ||
        operationState_ != LiveRecordingOperationState::Finalizing ||
        !pendingStart_ || pendingStart_->sessionId != sessionId ||
        !pendingCompletion_ || pendingCompletion_->session.id() != sessionId) {
        return;
    }
    const auto finalMessage = !result.hasValue()
                                  ? fromUtf8(result.error().message())
                                  : pendingTerminalError_
                                        ? fromUtf8(pendingTerminalError_->message())
                                        : tr("Stopped");
    const auto committedSessionId = pendingCompletion_->session.id();
    pendingCompletion_.reset();
    pendingTerminalError_.reset();
    pendingStart_.reset();
    emit activeRecordingChanged();
    setOperationState(LiveRecordingOperationState::Idle);
    setStatusMessage(finalMessage);
    if (result.hasValue()) {
        emit recordingCommitted(fromUtf8(committedSessionId.value()));
    }
}

void LiveRecordingController::pollDiagnostics() {
    if (!engine_) return;
    const auto snapshot = engine_->snapshot();
    const int tracks = boundedInt(snapshot.trackCount);
    const int segments = boundedInt(snapshot.segmentsPublished);
    const auto queued = static_cast<qulonglong>(snapshot.queuedItems);
    const auto dropped = static_cast<qulonglong>(snapshot.videoFramesDropped);
    const auto syncDropped =
        static_cast<qulonglong>(snapshot.syncVideoFramesDropped);
    const auto duplicated =
        static_cast<qulonglong>(snapshot.syncVideoFramesDuplicated);
    const double driftMilliseconds =
        static_cast<double>(snapshot.maximumAbsoluteDriftNanoseconds) / 1'000'000.0;
    const double correctionPpm = snapshot.audioCorrectionPpm;
    const auto disk = static_cast<qulonglong>(snapshot.availableDiskBytes.value_or(0));
    const auto encoder = snapshot.encoderName.empty()
                             ? (isRecording() ? tr("Selecting encoder")
                                              : QStringLiteral("Not active"))
                             : fromUtf8(snapshot.encoderName);
    QString duration = takeDuration_;
    if (pendingStart_ && (isRecording() ||
                          operationState_ == LiveRecordingOperationState::Finalizing)) {
        duration = formatDuration(clock_() - pendingStart_->startedAt);
    }
    if (trackCount_ == tracks && segmentCount_ == segments && queuedItems_ == queued &&
        droppedFrames_ == dropped && diskAvailableBytes_ == disk &&
        syncDroppedFrames_ == syncDropped && duplicatedFrames_ == duplicated &&
        maximumDriftMilliseconds_ == driftMilliseconds &&
        audioCorrectionPpm_ == correctionPpm &&
        encoderName_ == encoder && takeDuration_ == duration) {
        return;
    }
    trackCount_ = tracks;
    segmentCount_ = segments;
    queuedItems_ = queued;
    droppedFrames_ = dropped;
    syncDroppedFrames_ = syncDropped;
    duplicatedFrames_ = duplicated;
    maximumDriftMilliseconds_ = driftMilliseconds;
    audioCorrectionPpm_ = correctionPpm;
    diskAvailableBytes_ = disk;
    encoderName_ = encoder;
    takeDuration_ = duration;
    emit diagnosticsChanged();
}

void LiveRecordingController::setOperationState(LiveRecordingOperationState state) {
    if (operationState_ == state) return;
    const bool wasRecording = isRecording();
    operationState_ = state;
    emit operationStateChanged();
    if (wasRecording != isRecording()) emit recordingChanged();
}

void LiveRecordingController::setStatusMessage(QString message) {
    if (statusMessage_ == message) return;
    statusMessage_ = std::move(message);
    emit statusMessageChanged();
}

void LiveRecordingController::resetDiagnostics() {
    segmentCount_ = 0;
    trackCount_ = 0;
    queuedItems_ = 0;
    droppedFrames_ = 0;
    syncDroppedFrames_ = 0;
    duplicatedFrames_ = 0;
    maximumDriftMilliseconds_ = 0.0;
    audioCorrectionPpm_ = 0.0;
    diskAvailableBytes_ = 0;
    encoderName_ = tr("Selecting encoder");
    takeDuration_ = QStringLiteral("00:00:00");
    emit diagnosticsChanged();
}

}  // namespace creator::app
