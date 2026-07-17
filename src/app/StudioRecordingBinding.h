#pragma once

#include "app/LiveRecordingController.h"
#include "project_store/IStudioStore.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <vector>

namespace creator::app {

class StudioWorkflowController;

class StudioRecordingBinding final : public QObject {
    Q_OBJECT

public:
    using Completion = std::function<void(core::Result<void>)>;
    using ProjectUrlProvider = std::function<QUrl()>;

    struct WorkflowPorts final {
        std::function<void(domain::SessionId,
                           std::vector<project_store::RecordingSourceRole>,
                           core::TimestampNs, Completion)> prepare;
        std::function<void(Completion)> abort;
        std::function<void(Completion)> complete;
        std::function<void(QUrl, Completion)> open;
    };

    StudioRecordingBinding(LiveRecordingController& recording,
                           WorkflowPorts workflow,
                           ProjectUrlProvider projectUrl,
                           QObject* parent = nullptr);
    StudioRecordingBinding(LiveRecordingController& recording,
                           StudioWorkflowController& workflow,
                           ProjectUrlProvider projectUrl,
                           QObject* parent = nullptr);
    ~StudioRecordingBinding() override;

    void openProject(QUrl projectUrl);

signals:
    void timelineReconciled(QUrl projectUrl);

private:
    void handleRecordingCommitted();
    void handleRecordingAborted();

    QPointer<LiveRecordingController> recording_;
    WorkflowPorts workflow_;
    ProjectUrlProvider projectUrl_;
    quint64 completionGeneration_{0};
    quint64 openGeneration_{0};
};

}  // namespace creator::app
