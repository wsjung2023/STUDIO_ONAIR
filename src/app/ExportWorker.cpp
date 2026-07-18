#include "app/ExportWorker.h"

#include <QThread>

#include <chrono>
#include <utility>

namespace creator::app {
namespace {

bool terminal(edit_engine::RenderJobState state) noexcept {
    return state == edit_engine::RenderJobState::Completed ||
           state == edit_engine::RenderJobState::Failed ||
           state == edit_engine::RenderJobState::Cancelled;
}

}  // namespace

ExportWorker::ExportWorker(
    std::unique_ptr<edit_engine::IEditEngine> engine,
    std::shared_ptr<std::atomic_bool> cancellationRequested)
    : engine_(std::move(engine)),
      cancellationRequested_(std::move(cancellationRequested)) {}

void ExportWorker::start(edit_engine::RenderRequest request) {
    cancellationRequested_->store(false, std::memory_order_release);
    auto created = engine_->render(request);
    if (!created.hasValue()) {
        emit finished(false, static_cast<int>(edit_engine::RenderJobState::Failed),
                      QString::fromStdString(created.error().message()));
        return;
    }
    auto job = std::move(created).value();
    bool cancellationSent = false;
    for (;;) {
        if (cancellationRequested_->load(std::memory_order_acquire) &&
            !cancellationSent) {
            auto cancelled = job->cancel();
            cancellationSent = cancelled.hasValue();
        }
        auto progress = job->progress();
        if (!progress.hasValue()) {
            emit finished(false,
                          static_cast<int>(edit_engine::RenderJobState::Failed),
                          QString::fromStdString(progress.error().message()));
            return;
        }
        const auto& value = progress.value();
        emit progressChanged(
            static_cast<int>(value.state()), value.fraction(),
            value.renderedThrough().time_since_epoch().count(),
            value.totalDuration().count());
        if (terminal(value.state())) {
            emit finished(value.state() == edit_engine::RenderJobState::Completed,
                          static_cast<int>(value.state()), {});
            return;
        }
        QThread::msleep(10);
    }
}

}  // namespace creator::app
