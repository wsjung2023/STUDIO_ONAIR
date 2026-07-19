#include "app/ExportController.h"

#include "app/ExportWorker.h"
#include "app/RecentProjectRegistry.h"
#include "core/AppError.h"

#include <QMetaObject>

#include <utility>

namespace creator::app {

ExportController::ExportController(
    std::unique_ptr<edit_engine::IEditEngine> engine, QObject* parent)
    : ExportController(std::move(engine),
                       std::make_unique<LocalExportDestinationResolver>(), parent) {}

ExportController::ExportController(
    std::unique_ptr<edit_engine::IEditEngine> engine,
    std::unique_ptr<IExportDestinationResolver> destinations, QObject* parent)
    : QObject(parent),
      cancellationRequested_(std::make_shared<std::atomic_bool>(false)),
      worker_(new ExportWorker{std::move(engine), cancellationRequested_}),
      destinations_(std::move(destinations)) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ExportWorker::progressChanged, this,
            [this](int state, double fraction, qint64, qint64) {
                state_ = state;
                progress_ = fraction;
                emit progressChanged();
            });
    connect(worker_, &ExportWorker::finished, this,
            [this](bool success, int state, const QString& message) {
                state_ = state;
                if (success) progress_ = 1.0;
                busy_ = false;
                emit progressChanged();
                emit busyChanged();
                setStatus(message.isEmpty()
                              ? (success ? tr("Export completed")
                                         : tr("Export stopped"))
                              : message);
                emit exportFinished(success);
            });
    workerThread_.start();
}

ExportController::~ExportController() {
    cancellationRequested_->store(true, std::memory_order_release);
    disconnect(worker_, nullptr, this, nullptr);
    workerThread_.quit();
    workerThread_.wait();
}

void ExportController::setRequest(edit_engine::RenderRequest request) {
    if (!busy_) {
        request_ = std::move(request);
        publishAction_ = {};
    }
}

void ExportController::setSource(domain::ProjectId projectId,
                                 edit_engine::TimelineSnapshot snapshot) {
    source_ = Source{std::move(projectId), std::move(snapshot)};
    emit sourceChanged();
}

void ExportController::clearSource() {
    if (!source_.has_value()) return;
    source_.reset();
    request_.reset();
    publishAction_ = {};
    emit sourceChanged();
}

bool ExportController::canCancel() const noexcept {
    return busy_ &&
           state_ != static_cast<int>(edit_engine::RenderJobState::Publishing);
}

void ExportController::startExport() {
    if (busy_ || !request_.has_value()) {
        if (!request_.has_value()) setStatus(tr("No export request is ready"));
        return;
    }
    busy_ = true;
    progress_ = 0.0;
    state_ = static_cast<int>(edit_engine::RenderJobState::Pending);
    cancellationRequested_->store(false, std::memory_order_release);
    emit busyChanged();
    emit progressChanged();
    setStatus(tr("Preparing export"));
    const auto request = *request_;
    auto publish = std::move(publishAction_);
    QMetaObject::invokeMethod(worker_, [worker = worker_, request,
                                        publish = std::move(publish)]() mutable {
        worker->start(std::move(request), std::move(publish));
    }, Qt::QueuedConnection);
}

void ExportController::exportTo(const QUrl& destination,
                                const QString& presetId,
                                bool replaceExisting) {
    if (busy_) return;
    if (!source_.has_value()) {
        setStatus(tr("No editable timeline is ready"));
        return;
    }
    if (!destinations_) {
        setStatus(tr("Export destination service is unavailable"));
        return;
    }
    auto resolved = destinations_->resolve(destination, replaceExisting);
    if (!resolved.hasValue()) {
        setStatus(QString::fromStdString(resolved.error().message()));
        return;
    }
    core::Result<edit_engine::RenderPreset> preset =
        presetId == QStringLiteral("h264-1080p30")
            ? edit_engine::RenderPreset::h2641080p30()
            : presetId == QStringLiteral("h264-2160p30")
                  ? edit_engine::RenderPreset::h2642160p30()
                  : core::AppError{core::ErrorCode::InvalidArgument,
                                   "unsupported export preset"};
    if (!preset.hasValue()) {
        setStatus(QString::fromStdString(preset.error().message()));
        return;
    }
    auto request = edit_engine::RenderRequest::create(
        source_->projectId, source_->snapshot,
        resolved.value().renderPath,
        std::move(preset).value(),
        replaceExisting
            ? edit_engine::RenderOverwritePolicy::ReplaceExisting
            : edit_engine::RenderOverwritePolicy::FailIfExists);
    if (!request.hasValue()) {
        setStatus(QString::fromStdString(request.error().message()));
        return;
    }
    request_ = std::move(request).value();
    publishAction_ = std::move(resolved).value().publish;
    startExport();
}

void ExportController::cancelExport() {
    if (!canCancel()) return;
    cancellationRequested_->store(true, std::memory_order_release);
    setStatus(tr("Cancelling export"));
}

void ExportController::setStatus(QString message) {
    if (statusMessage_ == message) return;
    statusMessage_ = std::move(message);
    emit statusMessageChanged();
}

}  // namespace creator::app
