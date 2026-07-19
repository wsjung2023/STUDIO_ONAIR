#include "app/EditorSessionWorker.h"

#include "app/GeneratedOverlayCache.h"

#include "core/Uuid.h"
#include "domain/DeleteRangeCommand.h"
#include "domain/GeneratedClipCommands.h"
#include "domain/SetAudioEnvelopeCommand.h"
#include "domain/SetVisualTransformCommand.h"
#include "domain/SplitClipCommand.h"
#include "domain/TimelineRevision.h"
#include "domain/TrimClipCommand.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <string>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

using core::AppError;
using core::ErrorCode;
using core::FrameRate;
using domain::Timeline;
using domain::TimelineId;
using domain::TimelineRevision;
using domain::Track;
using domain::TrackId;
using domain::TrackKind;

AppError invalidRequest(std::string message) {
    return AppError{ErrorCode::InvalidArgument, std::move(message)};
}

core::Result<std::unique_ptr<domain::IEditCommand>> editCommand(
    const EditorEditRequest& request, const Timeline& timeline) {
    auto commandId = domain::CommandId::create(core::generateUuidV4());
    if (!commandId.hasValue()) return commandId.error();

    if (request.kind == EditorEditKind::Split) {
        if (!request.trackId.has_value() || !request.clipId.has_value()) {
            return invalidRequest("split requires a selected track and clip");
        }
        auto rightId = domain::ClipId::create(core::generateUuidV4());
        if (!rightId.hasValue()) return rightId.error();
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::SplitClipCommand>(
                std::move(commandId).value(), *request.trackId, *request.clipId,
                std::move(rightId).value(), request.position)};
    }
    if (request.kind == EditorEditKind::TrimLeading ||
        request.kind == EditorEditKind::TrimTrailing) {
        if (!request.trackId.has_value() || !request.clipId.has_value()) {
            return invalidRequest("trim requires a selected track and clip");
        }
        const auto edge = request.kind == EditorEditKind::TrimLeading
                              ? domain::TrimEdge::Leading
                              : domain::TrimEdge::Trailing;
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::TrimClipCommand>(
                std::move(commandId).value(), *request.trackId, *request.clipId,
                edge, request.position)};
    }
    if (request.kind == EditorEditKind::DeleteRange) {
        if (!request.range.has_value()) {
            return invalidRequest("delete range requires a non-empty range");
        }
        std::vector<domain::ClipId> rightIds;
        for (const auto& track : timeline.tracks()) {
            if (track.locked()) continue;
            for (const auto& clip : track.clips()) {
                if (clip.timelineRange().start() < request.range->start() &&
                    clip.timelineRange().end() > request.range->end()) {
                    auto rightId = domain::ClipId::create(core::generateUuidV4());
                    if (!rightId.hasValue()) return rightId.error();
                    rightIds.push_back(std::move(rightId).value());
                }
            }
        }
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::DeleteRangeCommand>(
                std::move(commandId).value(), *request.range, request.ripple,
                std::move(rightIds))};
    }
    if (request.kind == EditorEditKind::SetVisualTransform ||
        request.kind == EditorEditKind::SetAudioEnvelope) {
        if (!request.trackId.has_value() || !request.clipId.has_value()) {
            return invalidRequest(
                "effect edit requires a selected track and clip");
        }
        if (request.kind == EditorEditKind::SetVisualTransform) {
            return std::unique_ptr<domain::IEditCommand>{
                std::make_unique<domain::SetVisualTransformCommand>(
                    std::move(commandId).value(), *request.trackId,
                    *request.clipId, request.visualTransform)};
        }
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::SetAudioEnvelopeCommand>(
                std::move(commandId).value(), *request.trackId,
                *request.clipId, request.audioEnvelope)};
    }
    if (request.kind == EditorEditKind::AddTitle) {
        if (!request.range.has_value() || !request.titlePayload.has_value() ||
            request.trackId.has_value() || request.clipId.has_value()) {
            return invalidRequest(
                "add title requires a range and payload without generated identities");
        }
        auto trackId = domain::TrackId::create("title-1");
        if (!trackId.hasValue()) return trackId.error();
        auto clipId = domain::ClipId::create(core::generateUuidV4());
        if (!clipId.hasValue()) return clipId.error();
        auto clip = domain::Clip::createTitle(
            std::move(clipId).value(), *request.range, true,
            *request.titlePayload, request.visualTransform);
        if (!clip.hasValue()) return clip.error();
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::AddTitleCommand>(
                std::move(commandId).value(), std::move(trackId).value(),
                "Titles", std::move(clip).value())};
    }
    if (request.kind == EditorEditKind::EditTitle) {
        if (!request.trackId.has_value() || !request.clipId.has_value() ||
            !request.titlePayload.has_value()) {
            return invalidRequest(
                "edit title requires a selected title clip and payload");
        }
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::EditTitleCommand>(
                std::move(commandId).value(), *request.trackId,
                *request.clipId, *request.titlePayload)};
    }
    if (request.kind == EditorEditKind::RemoveGeneratedClip) {
        if (!request.trackId.has_value() || !request.clipId.has_value()) {
            return invalidRequest(
                "remove generated clip requires a selected track and clip");
        }
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::RemoveGeneratedClipCommand>(
                std::move(commandId).value(), *request.trackId,
                *request.clipId)};
    }
    if (request.kind == EditorEditKind::AddCaptionCue) {
        if (!request.captionCue.has_value() || request.cueId.has_value()) {
            return invalidRequest(
                "add caption cue requires cue content without a generated cue identity");
        }
        auto trackId = domain::TrackId::create("caption-1");
        if (!trackId.hasValue()) return trackId.error();
        if (request.trackId.has_value() && *request.trackId != trackId.value()) {
            return invalidRequest("caption cues require the stable caption track");
        }
        std::optional<domain::ClipId> clipId = request.clipId;
        std::optional<domain::TimeRange> clipRange = request.range;
        bool enabled = true;
        auto visual = request.visualTransform;
        if (request.clipId.has_value()) {
            if (request.range.has_value() || request.visualTransform.has_value()) {
                return invalidRequest(
                    "existing caption clip settings are derived by the worker");
            }
            const auto* existing =
                timeline.clip(trackId.value(), *request.clipId);
            if (existing == nullptr ||
                existing->kind() != domain::ClipKind::Caption) {
                return invalidRequest("selected caption clip was not found");
            }
            clipRange = existing->timelineRange();
            enabled = existing->enabled();
            visual = existing->visualTransform();
        } else if (!request.range.has_value()) {
            return invalidRequest("new caption clip requires a timeline range");
        } else {
            auto createdClipId =
                domain::ClipId::create(core::generateUuidV4());
            if (!createdClipId.hasValue()) return createdClipId.error();
            clipId = std::move(createdClipId).value();
        }
        auto cueId = domain::CueId::create(core::generateUuidV4());
        if (!cueId.hasValue()) return cueId.error();
        auto cue = domain::CaptionCue::create(
            std::move(cueId).value(), request.captionCue->startOffset,
            request.captionCue->duration, request.captionCue->text);
        if (!cue.hasValue()) return cue.error();
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::AddCaptionCueCommand>(
                std::move(commandId).value(), std::move(trackId).value(),
                "Captions", std::move(*clipId), *clipRange, enabled,
                std::move(visual), std::move(cue).value())};
    }
    if (request.kind == EditorEditKind::EditCaptionCue) {
        if (!request.trackId.has_value() || !request.clipId.has_value() ||
            !request.cueId.has_value() || !request.captionCue.has_value()) {
            return invalidRequest(
                "edit caption cue requires a selected cue and replacement");
        }
        auto replacement = domain::CaptionCue::create(
            *request.cueId, request.captionCue->startOffset,
            request.captionCue->duration, request.captionCue->text);
        if (!replacement.hasValue()) return replacement.error();
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::EditCaptionCueCommand>(
                std::move(commandId).value(), *request.trackId,
                *request.clipId, *request.cueId,
                std::move(replacement).value())};
    }
    if (request.kind == EditorEditKind::RemoveCaptionCue) {
        if (!request.trackId.has_value() || !request.clipId.has_value() ||
            !request.cueId.has_value()) {
            return invalidRequest(
                "remove caption cue requires a selected cue");
        }
        return std::unique_ptr<domain::IEditCommand>{
            std::make_unique<domain::RemoveCaptionCueCommand>(
                std::move(commandId).value(), *request.trackId,
                *request.clipId, *request.cueId)};
    }
    return invalidRequest("edit request kind does not create a command");
}

core::Result<Timeline> defaultTimeline() {
    auto timelineId = TimelineId::create("main");
    if (!timelineId.hasValue()) return timelineId.error();
    auto frameRate = FrameRate::create(60, 1);
    if (!frameRate.hasValue()) return frameRate.error();
    auto timeline = Timeline::create(std::move(timelineId).value(), "Main",
                                     frameRate.value());
    if (!timeline.hasValue()) return timeline.error();

    auto videoId = TrackId::create("video-1");
    if (!videoId.hasValue()) return videoId.error();
    auto video = Track::create(std::move(videoId).value(), TrackKind::Video,
                               "Video 1", true, false);
    if (!video.hasValue()) return video.error();
    auto addedVideo = timeline.value().addTrack(std::move(video).value());
    if (!addedVideo.hasValue()) return addedVideo.error();

    auto audioId = TrackId::create("audio-1");
    if (!audioId.hasValue()) return audioId.error();
    auto audio = Track::create(std::move(audioId).value(), TrackKind::Audio,
                               "Audio 1", true, false);
    if (!audio.hasValue()) return audio.error();
    auto addedAudio = timeline.value().addTrack(std::move(audio).value());
    if (!addedAudio.hasValue()) return addedAudio.error();
    return std::move(timeline).value();
}

std::filesystem::path pathFromUtf8(const std::string& value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const unsigned char byte : value) {
        utf8.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{utf8};
}

}  // namespace

EditorSessionWorker::EditorSessionWorker()
    : EditorSessionWorker(
          std::make_unique<project_store::ProjectPackageStore>(),
          [](const std::filesystem::path& databasePath,
             const domain::ProjectId& projectId)
              -> core::Result<
                  std::unique_ptr<project_store::ITimelineStore>> {
              auto store = project_store::SqliteTimelineStore::open(databasePath,
                                                                     projectId);
              if (!store.hasValue()) return store.error();
              return std::unique_ptr<project_store::ITimelineStore>{
                  std::make_unique<project_store::SqliteTimelineStore>(
                      std::move(store).value())};
          }, {}) {}

EditorSessionWorker::EditorSessionWorker(
    std::unique_ptr<project_store::IProjectPackageStore> packageStore,
    TimelineStoreFactory timelineStoreFactory,
    GeneratedOverlayFactory generatedOverlayFactory)
    : packageStore_(std::move(packageStore)),
      timelineStoreFactory_(std::move(timelineStoreFactory)),
      generatedOverlayFactory_(std::move(generatedOverlayFactory)) {
    if (!generatedOverlayFactory_) {
        generatedOverlayFactory_ =
            [](const edit_engine::TimelineSnapshot& snapshot)
            -> core::Result<GeneratedOverlayCacheResult> {
            GeneratedOverlayCache cache;
            return cache.synchronize(
                snapshot.mediaRoot, snapshot.timeline, snapshot.canvasWidth,
                snapshot.canvasHeight, snapshot.timeline.frameRate());
        };
    }
    qRegisterMetaType<EditorSessionResultPtr>();
}

void EditorSessionWorker::openProject(quint64 generation,
                                      std::filesystem::path packageRoot) {
    editService_.reset();
    timelineStore_.reset();
    packageRoot_.clear();
    assets_.clear();
    canvas_ = {};

    if (!packageStore_ || !timelineStoreFactory_) {
        publishError(generation,
                     AppError{ErrorCode::InvalidState,
                              "editor session dependencies are unavailable"});
        return;
    }

    auto openedProject = packageStore_->open(packageRoot);
    if (!openedProject.hasValue()) {
        publishError(generation, openedProject.error());
        return;
    }
    const auto& package = openedProject.value().package;
    const auto databasePath = package.path / pathFromUtf8(package.manifest.database);
    auto createdStore = timelineStoreFactory_(databasePath,
                                              package.manifest.projectId);
    if (!createdStore.hasValue()) {
        publishError(generation, createdStore.error());
        return;
    }
    timelineStore_ = std::move(createdStore).value();
    if (!timelineStore_) {
        publishError(generation,
                     AppError{ErrorCode::InvalidState,
                              "timeline store factory returned no store"});
        return;
    }

    auto persisted = timelineStore_->loadPrimaryTimeline();
    if (!persisted.hasValue()) {
        if (persisted.error().code() != ErrorCode::NotFound) {
            publishError(generation, persisted.error());
            return;
        }
        auto timeline = defaultTimeline();
        if (!timeline.hasValue()) {
            publishError(generation, timeline.error());
            return;
        }
        auto created = timelineStore_->createTimeline(timeline.value());
        if (!created.hasValue()) {
            publishError(generation, created.error());
            return;
        }
    }

    auto service = TimelineEditService::open(
        *timelineStore_, 100, [] { return core::generateUuidV4(); },
        [] { return core::Utc::now(); });
    if (!service.hasValue()) {
        publishError(generation, service.error());
        return;
    }
    editService_.emplace(std::move(service).value());

    auto assets = timelineStore_->assets();
    if (!assets.hasValue()) {
        publishError(generation, assets.error());
        return;
    }
    packageRoot_ = package.path;
    assets_ = std::move(assets).value();
    canvas_ = package.manifest.canvas;
    std::optional<AppError> derivedDiagnostic;
    auto state = currentState(&derivedDiagnostic);
    if (!state.hasValue()) {
        publishError(generation, state.error());
        return;
    }
    emit opened(generation, std::make_shared<const EditorSessionResult>(
                                EditorSessionUpdate{
                                    .state = std::move(state).value(),
                                    .derivedWorkDiagnostic =
                                        std::move(derivedDiagnostic)}));
}

void EditorSessionWorker::edit(quint64 generation, quint64 commandId,
                               EditorEditRequest request) {
    if (!editService_.has_value()) {
        publishEditError(
            generation, commandId,
            AppError{ErrorCode::InvalidState, "editor session is not open"});
        return;
    }

    auto baseRevision = TimelineRevision::create(editService_->revision());
    if (!baseRevision.hasValue()) {
        publishEditError(generation, commandId, baseRevision.error());
        return;
    }

    core::Result<void> applied = core::ok();
    if (request.kind == EditorEditKind::Undo) {
        applied = editService_->undo();
    } else if (request.kind == EditorEditKind::Redo) {
        applied = editService_->redo();
    } else if (request.kind == EditorEditKind::Save) {
        applied = editService_->markExplicitSave();
    } else {
        auto command = editCommand(request, editService_->snapshot());
        if (!command.hasValue()) {
            publishEditError(generation, commandId, command.error());
            return;
        }
        applied = editService_->execute(std::move(command).value());
    }
    if (!applied.hasValue()) {
        publishEditError(generation, commandId, applied.error());
        return;
    }

    std::optional<AppError> derivedDiagnostic;
    auto state = currentState(&derivedDiagnostic);
    if (!state.hasValue()) {
        publishEditError(generation, commandId, state.error());
        return;
    }
    std::optional<edit_engine::TimelineChangeSet> change;
    if (request.kind != EditorEditKind::Save) {
        std::vector<domain::TrackId> affectedTracks;
        bool requiresFullRebuild = false;
        if (request.kind == EditorEditKind::Split ||
            request.kind == EditorEditKind::TrimLeading ||
            request.kind == EditorEditKind::TrimTrailing ||
            request.kind == EditorEditKind::SetVisualTransform ||
            request.kind == EditorEditKind::SetAudioEnvelope) {
            if (!request.trackId.has_value()) {
                publishEditError(
                    generation, commandId,
                    invalidRequest("clip edit result has no affected track"));
                return;
            }
            affectedTracks.push_back(*request.trackId);
        } else {
            requiresFullRebuild = true;
        }
        auto createdChange = edit_engine::TimelineChangeSet::create(
            baseRevision.value(), state.value().snapshot,
            std::move(affectedTracks), requiresFullRebuild);
        if (!createdChange.hasValue()) {
            publishEditError(generation, commandId, createdChange.error());
            return;
        }
        change.emplace(std::move(createdChange).value());
    }
    emit edited(generation, commandId,
                std::make_shared<const EditorSessionResult>(
                    EditorSessionUpdate{.state = std::move(state).value(),
                                        .change = std::move(change),
                                        .derivedWorkDiagnostic =
                                            std::move(derivedDiagnostic)}));
}

void EditorSessionWorker::publishError(quint64 generation, AppError error) {
    editService_.reset();
    timelineStore_.reset();
    packageRoot_.clear();
    assets_.clear();
    canvas_ = {};
    emit opened(generation, std::make_shared<const EditorSessionResult>(
                                std::move(error)));
}

void EditorSessionWorker::publishEditError(quint64 generation, quint64 commandId,
                                           AppError error) {
    emit edited(generation, commandId,
                std::make_shared<const EditorSessionResult>(std::move(error)));
}

core::Result<EditorSessionState> EditorSessionWorker::currentState(
    std::optional<AppError>* derivedWorkDiagnostic) const {
    if (!editService_.has_value() || packageRoot_.empty()) {
        return AppError{ErrorCode::InvalidState, "editor session is not open"};
    }
    auto revision = TimelineRevision::create(editService_->revision());
    if (!revision.hasValue()) return revision.error();
    auto snapshot = edit_engine::TimelineSnapshot{
        editService_->snapshot(), revision.value(), assets_, packageRoot_,
        canvas_.width, canvas_.height};
    if (auto validated = edit_engine::validateTimelineSnapshot(snapshot);
        !validated.hasValue()) {
        return validated.error();
    }
    auto generated = generatedOverlayFactory_(snapshot);
    if (generated.hasValue()) {
        auto generatedResult = std::move(generated).value();
        snapshot.generatedOverlays = std::move(generatedResult.descriptors);
        if (!generatedResult.diagnostics.empty() &&
            derivedWorkDiagnostic != nullptr) {
            *derivedWorkDiagnostic =
                std::move(generatedResult.diagnostics.front());
        }
        if (auto validated = edit_engine::validateTimelineSnapshot(snapshot);
            !validated.hasValue()) {
            if (derivedWorkDiagnostic != nullptr) {
                *derivedWorkDiagnostic = validated.error();
            }
            snapshot.generatedOverlays.clear();
        }
    } else if (derivedWorkDiagnostic != nullptr) {
        *derivedWorkDiagnostic = generated.error();
    }
    return EditorSessionState{
        .assets = assets_,
        .snapshot = std::move(snapshot),
        .canUndo = editService_->canUndo(),
        .canRedo = editService_->canRedo(),
        .clean = editService_->isClean(),
        .historyCursor = editService_->historyCursor(),
    };
}

}  // namespace creator::app
