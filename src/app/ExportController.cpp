#include "app/ExportController.h"

#include "app/ExportWorker.h"

#include <QMetaObject>

#include <utility>

namespace creator::app {

ExportController::ExportController(
    std::unique_ptr<edit_engine::IEditEngine> engine, QObject* parent)
    : QObject(parent),
      cancellationRequested_(std::make_shared<std::atomic_bool>(false)),
      worker_(new ExportWorker{std::move(engine), cancellationRequested_}) {
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
    if (!busy_) request_ = std::move(request);
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
    QMetaObject::invokeMethod(worker_, [worker = worker_, request]() mutable {
        worker->start(std::move(request));
    }, Qt::QueuedConnection);
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
