#include "app/StudioRecordingBinding.h"

#include "app/StudioWorkflowController.h"
#include "core/AppError.h"

#include <QPointer>

#include <utility>

namespace creator::app {
namespace {

core::Result<domain::StudioSourceRole> studioRole(
    recorder::TrackRole role) {
    switch (role) {
    case recorder::TrackRole::Screen:
        return domain::StudioSourceRole::Screen;
    case recorder::TrackRole::Camera:
        return domain::StudioSourceRole::Camera;
    case recorder::TrackRole::Avatar:
        return domain::StudioSourceRole::Avatar;
    case recorder::TrackRole::Microphone:
        return domain::StudioSourceRole::Microphone;
    case recorder::TrackRole::SystemAudio:
        return domain::StudioSourceRole::SystemAudio;
    case recorder::TrackRole::CompositePreview:
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Composite preview cannot be recorded as a Studio source"};
    }
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "Live recording source role is invalid"};
}

StudioRecordingBinding::WorkflowPorts portsFor(
    StudioWorkflowController& workflow) {
    QPointer<StudioWorkflowController> guarded{&workflow};
    return {
        .prepare = [guarded](
                       domain::SessionId sessionId,
                       std::vector<project_store::RecordingSourceRole> sources,
                       core::TimestampNs position,
                       StudioRecordingBinding::Completion completion) mutable {
            if (!guarded) {
                completion(core::AppError{core::ErrorCode::InvalidState,
                                          "Studio workflow is unavailable"});
                return;
            }
            guarded->prepareRecording(std::move(sessionId), std::move(sources),
                                      position, std::move(completion));
        },
        .abort = [guarded](StudioRecordingBinding::Completion completion) mutable {
            if (!guarded) return;
            guarded->abortRecording(std::move(completion));
        },
        .complete = [guarded](StudioRecordingBinding::Completion completion) mutable {
            if (!guarded) return;
            guarded->completeRecording(std::move(completion));
        },
        .open = [guarded](QUrl projectUrl,
                          StudioRecordingBinding::Completion completion) mutable {
            if (!guarded) {
                completion(core::AppError{core::ErrorCode::InvalidState,
                                          "Studio workflow is unavailable"});
                return;
            }
            guarded->openProject(std::move(projectUrl), std::move(completion));
        }};
}

}  // namespace

StudioRecordingBinding::StudioRecordingBinding(
    LiveRecordingController& recording, WorkflowPorts workflow,
    ProjectUrlProvider projectUrl, QObject* parent)
    : QObject(parent),
      recording_(&recording),
      workflow_(std::move(workflow)),
      projectUrl_(std::move(projectUrl)) {
    QPointer<StudioRecordingBinding> self{this};
    recording.setRecordingPreparation(
        [self](const LiveRecordingStart& start,
               LiveRecordingController::PreparationCompletion completion) mutable {
            if (!self || !self->workflow_.prepare) {
                completion(core::AppError{
                    core::ErrorCode::InvalidState,
                    "Studio recording preparation is unavailable"});
                return;
            }
            std::vector<project_store::RecordingSourceRole> mappings;
            mappings.reserve(start.sources.size());
            for (const auto& source : start.sources) {
                auto role = studioRole(source.role);
                if (!role.hasValue()) {
                    completion(role.error());
                    return;
                }
                mappings.push_back({.sourceId = source.sourceId,
                                    .role = role.value()});
            }
            self->workflow_.prepare(start.sessionId,
                                    std::move(mappings),
                                    core::TimestampNs{},
                                    std::move(completion));
        });
    connect(&recording, &LiveRecordingController::recordingCommitted,
            this, [this] { handleRecordingCommitted(); });
    connect(&recording, &LiveRecordingController::recordingAborted,
            this, [this] { handleRecordingAborted(); });
}

StudioRecordingBinding::StudioRecordingBinding(
    LiveRecordingController& recording, StudioWorkflowController& workflow,
    ProjectUrlProvider projectUrl, QObject* parent)
    : StudioRecordingBinding(recording, portsFor(workflow),
                             std::move(projectUrl), parent) {}

StudioRecordingBinding::~StudioRecordingBinding() {
    ++completionGeneration_;
    ++openGeneration_;
    if (recording_) recording_->setRecordingPreparation({});
}

void StudioRecordingBinding::handleRecordingCommitted() {
    if (!workflow_.complete) return;
    const auto generation = ++completionGeneration_;
    QPointer<StudioRecordingBinding> self{this};
    workflow_.complete([self, generation](core::Result<void> result) {
        if (!self || generation != self->completionGeneration_ ||
            !result.hasValue()) return;
        const auto projectUrl = self->projectUrl_ ? self->projectUrl_() : QUrl{};
        if (projectUrl.isValid() && !projectUrl.isEmpty()) {
            emit self->timelineReconciled(projectUrl);
        }
    });
}

void StudioRecordingBinding::handleRecordingAborted() {
    if (!workflow_.abort) return;
    ++completionGeneration_;
    workflow_.abort([](core::Result<void>) {});
}

void StudioRecordingBinding::openProject(QUrl projectUrl) {
    if (!workflow_.open) return;
    const auto generation = ++openGeneration_;
    QPointer<StudioRecordingBinding> self{this};
    workflow_.open(
        projectUrl,
        [self, generation, projectUrl = std::move(projectUrl)](
            core::Result<void> result) mutable {
            if (!self || generation != self->openGeneration_ ||
                !result.hasValue()) return;
            emit self->timelineReconciled(std::move(projectUrl));
        });
}

}  // namespace creator::app
