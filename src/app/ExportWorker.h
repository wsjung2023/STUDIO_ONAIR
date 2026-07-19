#pragma once

#include "edit_engine/IEditEngine.h"
#include "app/IExportDestinationResolver.h"

#include <QObject>

#include <atomic>
#include <memory>

namespace creator::app {

class ExportWorker final : public QObject {
    Q_OBJECT
public:
    ExportWorker(std::unique_ptr<edit_engine::IEditEngine> engine,
                 std::shared_ptr<std::atomic_bool> cancellationRequested);

    void start(edit_engine::RenderRequest request,
               ExportPublishAction publish = {});

signals:
    void progressChanged(int state, double fraction, qint64 renderedNs,
                         qint64 totalNs);
    void finished(bool success, int state, QString message);

private:
    std::unique_ptr<edit_engine::IEditEngine> engine_;
    std::shared_ptr<std::atomic_bool> cancellationRequested_;
};

}  // namespace creator::app
