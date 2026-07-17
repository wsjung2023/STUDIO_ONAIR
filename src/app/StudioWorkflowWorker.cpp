#include "app/StudioWorkflowWorker.h"

#include "core/AppError.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;
using domain::SceneId;
using domain::SceneSource;
using domain::SourceId;
using domain::StudioScene;
using domain::StudioSourceRole;
using domain::VisualTransform;
using project_store::StudioSnapshot;

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidArgument, std::move(message)};
}

AppError invalidState(std::string message) {
    return AppError{ErrorCode::InvalidState, std::move(message)};
}

template <typename Range, typename Id, typename Projection>
auto findById(Range& values, const Id& id, Projection projection) {
    return std::ranges::find(values, id, projection);
}

Result<VisualTransform> fullFrame() {
    return VisualTransform::create(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
                                   0.0, 0.0, 0.0, 0.0, 1.0, 0);
}

Result<VisualTransform> pipPreset(const std::string& preset) {
    double x = 0.70;
    double y = 0.05;
    if (preset == "top-left") {
        x = 0.05;
    } else if (preset == "bottom-left") {
        x = 0.05;
        y = 0.70;
    } else if (preset == "bottom-right") {
        y = 0.70;
    } else if (preset != "top-right") {
        return invalid("unknown Studio PIP preset");
    }
    return VisualTransform::create(x, y, 0.25, 0.25, 1.0, 1.0, 0.0,
                                   0.0, 0.0, 0.0, 0.0, 1.0, 10);
}

Result<std::vector<StudioScene>> normalizedScenes(
    std::vector<StudioScene> scenes) {
    std::ranges::sort(scenes, {}, &StudioScene::position);
    std::vector<StudioScene> normalized;
    normalized.reserve(scenes.size());
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        if (index > static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max())) {
            return invalid("Studio scene position overflow");
        }
        auto positioned =
            scenes[index].withPosition(static_cast<std::int32_t>(index));
        if (!positioned.hasValue()) return positioned.error();
        normalized.push_back(std::move(positioned).value());
    }
    return normalized;
}

}  // namespace

StudioWorkflowWorker::StudioWorkflowWorker(
    StudioStoreOpenFactory storeFactory,
    std::unique_ptr<IRecordingTimelineReconciler> reconciler,
    StudioIdentityFactory identityFactory)
    : storeFactory_(std::move(storeFactory)),
      reconciler_(std::move(reconciler)),
      identityFactory_(std::move(identityFactory)) {}

void StudioWorkflowWorker::emitResult(quint64 generation, quint64 commandId,
                                      StudioWorkflowResult result) {
    emit completed(generation, commandId,
                   std::make_shared<const StudioWorkflowResult>(
                       std::move(result)));
}

void StudioWorkflowWorker::openProject(quint64 generation,
                                       std::filesystem::path packageRoot) {
    activeGeneration_ = generation;
    state_.reset();
    store_.reset();
    packageRoot_.clear();
    auto opened = storeFactory_(packageRoot);
    if (!opened.hasValue()) {
        emitResult(generation, 0, opened.error());
        return;
    }
    auto store = std::move(opened).value();
    if (!store) {
        emitResult(generation, 0,
                   invalid("Studio store factory returned no store"));
        return;
    }
    auto defaults = domain::defaultStudioScenes();
    if (!defaults.hasValue()) {
        emitResult(generation, 0, defaults.error());
        return;
    }
    if (auto seeded = store->seedDefaultsIfEmpty(defaults.value());
        !seeded.hasValue()) {
        emitResult(generation, 0, seeded.error());
        return;
    }
    auto loaded = store->load();
    if (!loaded.hasValue()) {
        emitResult(generation, 0, loaded.error());
        return;
    }
    if (loaded.value().scenes.empty() ||
        std::ranges::find(loaded.value().scenes,
                          loaded.value().activeSceneId,
                          &StudioScene::id) == loaded.value().scenes.end()) {
        emitResult(generation, 0,
                   AppError{ErrorCode::IoFailure,
                            "persisted Studio snapshot is incomplete"});
        return;
    }

    StudioWorkflowState staged{
        .snapshot = std::move(loaded).value(),
        .selectedSceneId = std::nullopt,
        .selectedSourceId = std::nullopt};
    staged.selectedSceneId = staged.snapshot.activeSceneId;
    const auto selected = std::ranges::find(
        staged.snapshot.scenes, staged.snapshot.activeSceneId,
        &StudioScene::id);
    if (selected != staged.snapshot.scenes.end() &&
        !selected->sources().empty()) {
        staged.selectedSourceId = selected->sources().front().id();
    }

    store_ = std::move(store);
    packageRoot_ = std::move(packageRoot);
    auto pending = store_->completedUnimportedRecordings();
    if (!pending.hasValue()) {
        staged.status = pending.error().message();
    } else {
        for (const auto& recording : pending.value()) {
            auto reconciled = reconcileSession(recording.sessionId, staged);
            if (!reconciled.hasValue()) {
                staged.status = reconciled.error().message();
                staged.reconciliationSessionId = recording.sessionId;
                break;
            }
        }
    }
    state_ = staged;
    emitResult(generation, 0, std::move(staged));
}

Result<void> StudioWorkflowWorker::publishSceneSnapshot(
    StudioSnapshot snapshot, StudioWorkflowState& staged) {
    auto committed = store_->commitSceneMutation(snapshot);
    if (!committed.hasValue()) return committed.error();
    staged.snapshot = std::move(snapshot);
    return core::ok();
}

Result<void> StudioWorkflowWorker::reconcileSession(
    const domain::SessionId& sessionId, StudioWorkflowState& staged) {
    if (!reconciler_) return invalidState("recording reconciler is unavailable");
    staged.reconciling = true;
    emit reconciliationProgress(activeGeneration_, true);
    auto reconciled = reconciler_->reconcile(packageRoot_, sessionId);
    staged.reconciling = false;
    emit reconciliationProgress(activeGeneration_, false);
    if (!reconciled.hasValue()) return reconciled.error();
    staged.reconciliationSessionId.reset();
    return core::ok();
}

Result<StudioWorkflowState> StudioWorkflowWorker::apply(
    const StudioWorkflowRequest& request) {
    if (!store_ || !state_.has_value()) {
        return invalidState("Studio project is not open");
    }
    StudioWorkflowState staged = *state_;
    staged.status.clear();

    auto sceneId = SceneId::create(request.sceneId);
    auto sourceId = SourceId::create(request.sourceId);
    auto selectedScene = [&]() -> StudioScene* {
        if (!staged.selectedSceneId.has_value()) return nullptr;
        const auto found = findById(staged.snapshot.scenes,
                                    *staged.selectedSceneId,
                                    &StudioScene::id);
        return found == staged.snapshot.scenes.end() ? nullptr : &*found;
    };

    const auto requireIdleEdit = [&]() -> Result<void> {
        if (staged.recording) {
            return invalidState("Studio layout is read-only while recording");
        }
        return core::ok();
    };

    switch (request.operation) {
    case StudioWorkflowOperation::AddScene: {
        if (auto guard = requireIdleEdit(); !guard.hasValue()) return guard.error();
        const auto identity = identityFactory_();
        auto id = SceneId::create(identity);
        if (!id.hasValue()) return id.error();
        if (staged.snapshot.scenes.size() >= 1024U) {
            return invalidState("Studio scene limit reached");
        }
        auto added = StudioScene::create(
            std::move(id).value(), request.text.empty() ? "Scene" : request.text,
            static_cast<std::int32_t>(staged.snapshot.scenes.size()), {});
        if (!added.hasValue()) return added.error();
        auto snapshot = staged.snapshot;
        snapshot.scenes.push_back(std::move(added).value());
        if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
            !committed.hasValue()) return committed.error();
        staged.selectedSceneId = staged.snapshot.scenes.back().id();
        staged.selectedSourceId.reset();
        break;
    }
    case StudioWorkflowOperation::DuplicateScene: {
        if (auto guard = requireIdleEdit(); !guard.hasValue()) return guard.error();
        const StudioScene* original = nullptr;
        if (!request.sceneId.empty() && sceneId.hasValue()) {
            const auto found = findById(staged.snapshot.scenes, sceneId.value(),
                                        &StudioScene::id);
            if (found != staged.snapshot.scenes.end()) original = &*found;
        } else {
            original = selectedScene();
        }
        if (!original) return invalidState("no Studio scene is selected");
        auto id = SceneId::create(identityFactory_());
        if (!id.hasValue()) return id.error();
        auto duplicate = StudioScene::create(
            std::move(id).value(), original->name() + " Copy",
            static_cast<std::int32_t>(staged.snapshot.scenes.size()),
            original->sources());
        if (!duplicate.hasValue()) return duplicate.error();
        auto snapshot = staged.snapshot;
        snapshot.scenes.push_back(std::move(duplicate).value());
        if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
            !committed.hasValue()) return committed.error();
        staged.selectedSceneId = staged.snapshot.scenes.back().id();
        staged.selectedSourceId = staged.snapshot.scenes.back().sources().empty()
                                      ? std::optional<SourceId>{}
                                      : std::optional<SourceId>{
                                            staged.snapshot.scenes.back()
                                                .sources()
                                                .front()
                                                .id()};
        break;
    }
    case StudioWorkflowOperation::RenameScene: {
        if (auto guard = requireIdleEdit(); !guard.hasValue()) return guard.error();
        if (!sceneId.hasValue()) return sceneId.error();
        auto snapshot = staged.snapshot;
        auto found = findById(snapshot.scenes, sceneId.value(), &StudioScene::id);
        if (found == snapshot.scenes.end()) {
            return AppError{ErrorCode::NotFound, "Studio scene was not found"};
        }
        auto renamed = found->withName(request.text);
        if (!renamed.hasValue()) return renamed.error();
        *found = std::move(renamed).value();
        if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
            !committed.hasValue()) return committed.error();
        break;
    }
    case StudioWorkflowOperation::RemoveScene: {
        if (auto guard = requireIdleEdit(); !guard.hasValue()) return guard.error();
        if (!sceneId.hasValue()) return sceneId.error();
        if (staged.snapshot.scenes.size() <= 1U) {
            return invalidState("the final Studio scene cannot be removed");
        }
        auto snapshot = staged.snapshot;
        const auto removed = std::erase_if(
            snapshot.scenes,
            [&](const StudioScene& value) { return value.id() == sceneId.value(); });
        if (removed != 1U) {
            return AppError{ErrorCode::NotFound, "Studio scene was not found"};
        }
        auto normalized = normalizedScenes(std::move(snapshot.scenes));
        if (!normalized.hasValue()) return normalized.error();
        snapshot.scenes = std::move(normalized).value();
        if (snapshot.activeSceneId == sceneId.value()) {
            snapshot.activeSceneId = snapshot.scenes.front().id();
        }
        if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
            !committed.hasValue()) return committed.error();
        if (staged.selectedSceneId == sceneId.value()) {
            staged.selectedSceneId = staged.snapshot.activeSceneId;
            const auto* current = selectedScene();
            staged.selectedSourceId =
                current && !current->sources().empty()
                    ? std::optional<SourceId>{current->sources().front().id()}
                    : std::optional<SourceId>{};
        }
        break;
    }
    case StudioWorkflowOperation::MoveScene: {
        if (auto guard = requireIdleEdit(); !guard.hasValue()) return guard.error();
        if (!sceneId.hasValue() ||
            (request.direction != -1 && request.direction != 1)) {
            return invalid("invalid Studio scene move");
        }
        auto snapshot = staged.snapshot;
        auto found = findById(snapshot.scenes, sceneId.value(), &StudioScene::id);
        if (found == snapshot.scenes.end()) {
            return AppError{ErrorCode::NotFound, "Studio scene was not found"};
        }
        const auto index = static_cast<std::ptrdiff_t>(
            std::distance(snapshot.scenes.begin(), found));
        const auto target = index + request.direction;
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(snapshot.scenes.size())) {
            return invalidState("Studio scene is already at the boundary");
        }
        std::swap(snapshot.scenes[static_cast<std::size_t>(index)],
                  snapshot.scenes[static_cast<std::size_t>(target)]);
        for (std::size_t position = 0; position < snapshot.scenes.size(); ++position) {
            auto value = snapshot.scenes[position].withPosition(
                static_cast<std::int32_t>(position));
            if (!value.hasValue()) return value.error();
            snapshot.scenes[position] = std::move(value).value();
        }
        if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
            !committed.hasValue()) return committed.error();
        break;
    }
    case StudioWorkflowOperation::SwitchScene: {
        if (!sceneId.hasValue()) return sceneId.error();
        if (findById(staged.snapshot.scenes, sceneId.value(), &StudioScene::id) ==
            staged.snapshot.scenes.end()) {
            return AppError{ErrorCode::NotFound, "Studio scene was not found"};
        }
        if (staged.recording) {
            if (!staged.activeSessionId.has_value()) {
                return invalidState("recording session identity is missing");
            }
            if (request.position.time_since_epoch() < core::DurationNs::zero() ||
                request.position < staged.recordingPosition) {
                return invalid("recording scene position must be monotonic");
            }
            if (staged.nextSceneEventSequence ==
                std::numeric_limits<std::uint64_t>::max()) {
                return invalidState("recording scene sequence is exhausted");
            }
            auto recorded = store_->recordSceneSwitch(
                *staged.activeSessionId, sceneId.value(),
                staged.nextSceneEventSequence, request.position);
            if (!recorded.hasValue()) return recorded.error();
            ++staged.nextSceneEventSequence;
            staged.recordingPosition = request.position;
            staged.snapshot.activeSceneId = sceneId.value();
        } else {
            auto snapshot = staged.snapshot;
            snapshot.activeSceneId = sceneId.value();
            if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
                !committed.hasValue()) return committed.error();
        }
        break;
    }
    case StudioWorkflowOperation::ToggleSource:
    case StudioWorkflowOperation::MoveSource:
    case StudioWorkflowOperation::SetSourceTransform:
    case StudioWorkflowOperation::SetSourcePipPreset:
    case StudioWorkflowOperation::ResetSourceTransform: {
        if (auto guard = requireIdleEdit(); !guard.hasValue()) return guard.error();
        if (!sceneId.hasValue()) return sceneId.error();
        if (!sourceId.hasValue()) return sourceId.error();
        auto snapshot = staged.snapshot;
        auto scene = findById(snapshot.scenes, sceneId.value(), &StudioScene::id);
        if (scene == snapshot.scenes.end()) {
            return AppError{ErrorCode::NotFound, "Studio scene was not found"};
        }
        auto sources = scene->sources();
        auto source = findById(sources, sourceId.value(), &SceneSource::id);
        if (source == sources.end()) {
            return AppError{ErrorCode::NotFound, "Studio source was not found"};
        }
        if (request.operation == StudioWorkflowOperation::ToggleSource) {
            if (!source->enabled() && !source->transform().has_value() &&
                (source->role() == StudioSourceRole::Screen ||
                 source->role() == StudioSourceRole::Camera)) {
                auto transform = fullFrame();
                if (!transform.hasValue()) return transform.error();
                auto transformed = source->withTransform(transform.value());
                if (!transformed.hasValue()) return transformed.error();
                *source = std::move(transformed).value();
            }
            auto toggled = source->withEnabled(!source->enabled());
            if (!toggled.hasValue()) return toggled.error();
            *source = std::move(toggled).value();
        } else if (request.operation == StudioWorkflowOperation::MoveSource) {
            if (request.direction != -1 && request.direction != 1) {
                return invalid("invalid Studio source move");
            }
            const auto index = static_cast<std::ptrdiff_t>(
                std::distance(sources.begin(), source));
            const auto target = index + request.direction;
            if (target < 0 || target >= static_cast<std::ptrdiff_t>(sources.size())) {
                return invalidState("Studio source is already at the boundary");
            }
            std::swap(sources[static_cast<std::size_t>(index)],
                      sources[static_cast<std::size_t>(target)]);
            for (std::size_t position = 0; position < sources.size(); ++position) {
                auto value = sources[position].withPosition(
                    static_cast<std::int32_t>(position));
                if (!value.hasValue()) return value.error();
                sources[position] = std::move(value).value();
            }
        } else {
            if (source->role() != StudioSourceRole::Screen &&
                source->role() != StudioSourceRole::Camera) {
                return invalid("audio sources have no visual transform");
            }
            if (request.operation ==
                    StudioWorkflowOperation::SetSourceTransform &&
                !request.transform.has_value()) {
                return invalid("Studio source transform is missing");
            }
            Result<VisualTransform> transform =
                request.operation == StudioWorkflowOperation::SetSourceTransform
                    ? Result<VisualTransform>{*request.transform}
                    : (request.operation ==
                               StudioWorkflowOperation::SetSourcePipPreset
                           ? pipPreset(request.text)
                           : fullFrame());
            if (!transform.hasValue()) return transform.error();
            auto transformed = source->withTransform(transform.value());
            if (!transformed.hasValue()) return transformed.error();
            *source = std::move(transformed).value();
        }
        auto updated = StudioScene::create(scene->id(), scene->name(),
                                           scene->position(), std::move(sources));
        if (!updated.hasValue()) return updated.error();
        *scene = std::move(updated).value();
        if (auto committed = publishSceneSnapshot(std::move(snapshot), staged);
            !committed.hasValue()) return committed.error();
        break;
    }
    case StudioWorkflowOperation::PrepareRecording: {
        if (staged.recording || !request.sessionId.has_value() ||
            request.recordingSources.empty() ||
            request.position.time_since_epoch() != core::DurationNs::zero()) {
            return invalidState("Studio recording cannot be prepared now");
        }
        auto prepared = store_->prepareRecording(
            *request.sessionId, request.recordingSources,
            staged.snapshot.activeSceneId);
        if (!prepared.hasValue()) return prepared.error();
        staged.recording = true;
        staged.activeSessionId = request.sessionId;
        staged.reconciliationSessionId.reset();
        staged.recordingPosition = request.position;
        staged.nextSceneEventSequence = 1;
        staged.markerCount = 0;
        break;
    }
    case StudioWorkflowOperation::AbortRecording: {
        if (!staged.recording || !staged.activeSessionId.has_value()) {
            return invalidState("Studio is not recording");
        }
        auto discarded =
            store_->discardRecordingPreparation(*staged.activeSessionId);
        if (!discarded.hasValue()) return discarded.error();
        staged.recording = false;
        staged.activeSessionId.reset();
        staged.recordingPosition = {};
        staged.markerCount = 0;
        staged.nextSceneEventSequence = 1;
        break;
    }
    case StudioWorkflowOperation::CompleteRecording: {
        if (!staged.recording || !staged.activeSessionId.has_value()) {
            return invalidState("Studio is not recording");
        }
        const auto completedSession = *staged.activeSessionId;
        staged.recording = false;
        staged.activeSessionId.reset();
        staged.reconciliationSessionId = completedSession;
        auto reconciled = reconcileSession(completedSession, staged);
        if (!reconciled.hasValue()) {
            staged.status = "Recording saved; timeline reconciliation failed: " +
                            reconciled.error().message();
        }
        break;
    }
    case StudioWorkflowOperation::AddMarker: {
        if (!staged.recording || !staged.activeSessionId.has_value()) {
            return invalidState("markers require an active recording");
        }
        if (request.position.time_since_epoch() < core::DurationNs::zero() ||
            request.position < staged.recordingPosition) {
            return invalid("recording marker position must be monotonic");
        }
        if (staged.markerCount == std::numeric_limits<std::size_t>::max()) {
            return invalidState("recording marker count is exhausted");
        }
        const auto markerId = identityFactory_();
        if (markerId.empty()) return invalid("marker identity is empty");
        project_store::RecordingMarker marker{
            .markerId = markerId,
            .sessionId = *staged.activeSessionId,
            .position = request.position,
            .label = request.text};
        auto recorded = store_->recordMarker(marker);
        if (!recorded.hasValue()) return recorded.error();
        ++staged.markerCount;
        staged.recordingPosition = request.position;
        break;
    }
    case StudioWorkflowOperation::RetryReconciliation: {
        if (staged.recording || !staged.reconciliationSessionId.has_value()) {
            return invalidState("no recording is awaiting reconciliation");
        }
        const auto retriedSession = *staged.reconciliationSessionId;
        auto reconciled = reconcileSession(retriedSession, staged);
        if (!reconciled.hasValue()) return reconciled.error();
        auto pending = store_->completedUnimportedRecordings();
        if (!pending.hasValue()) return pending.error();
        for (const auto& recording : pending.value()) {
            if (recording.sessionId == retriedSession) continue;
            auto next = reconcileSession(recording.sessionId, staged);
            if (!next.hasValue()) {
                staged.reconciliationSessionId = recording.sessionId;
                staged.status = next.error().message();
                break;
            }
        }
        break;
    }
    }
    state_ = staged;
    return staged;
}

void StudioWorkflowWorker::execute(quint64 generation, quint64 commandId,
                                   StudioWorkflowRequest request) {
    activeGeneration_ = generation;
    emitResult(generation, commandId, apply(request));
}

}  // namespace creator::app
