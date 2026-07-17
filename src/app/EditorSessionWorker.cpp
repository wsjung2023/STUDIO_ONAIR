#include "app/EditorSessionWorker.h"

#include "core/Uuid.h"
#include "domain/DeleteRangeCommand.h"
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
          }) {}

EditorSessionWorker::EditorSessionWorker(
    std::unique_ptr<project_store::IProjectPackageStore> packageStore,
    TimelineStoreFactory timelineStoreFactory)
    : packageStore_(std::move(packageStore)),
      timelineStoreFactory_(std::move(timelineStoreFactory)) {
    qRegisterMetaType<EditorSessionResultPtr>();
}

void EditorSessionWorker::openProject(quint64 generation,
                                      std::filesystem::path packageRoot) {
    editService_.reset();
    timelineStore_.reset();
    packageRoot_.clear();
    assets_.clear();

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
    auto state = currentState();
    if (!state.hasValue()) {
        publishError(generation, state.error());
        return;
    }
    emit opened(generation, std::make_shared<const EditorSessionResult>(
                                EditorSessionUpdate{
                                    .state = std::move(state).value()}));
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

    auto state = currentState();
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
            request.kind == EditorEditKind::TrimTrailing) {
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
                                        .change = std::move(change)}));
}

void EditorSessionWorker::publishError(quint64 generation, AppError error) {
    editService_.reset();
    timelineStore_.reset();
    packageRoot_.clear();
    assets_.clear();
    emit opened(generation, std::make_shared<const EditorSessionResult>(
                                std::move(error)));
}

void EditorSessionWorker::publishEditError(quint64 generation, quint64 commandId,
                                           AppError error) {
    emit edited(generation, commandId,
                std::make_shared<const EditorSessionResult>(std::move(error)));
}

core::Result<EditorSessionState> EditorSessionWorker::currentState() const {
    if (!editService_.has_value() || packageRoot_.empty()) {
        return AppError{ErrorCode::InvalidState, "editor session is not open"};
    }
    auto revision = TimelineRevision::create(editService_->revision());
    if (!revision.hasValue()) return revision.error();
    return EditorSessionState{
        .assets = assets_,
        .snapshot = edit_engine::TimelineSnapshot{
            editService_->snapshot(), revision.value(), assets_, packageRoot_},
        .canUndo = editService_->canUndo(),
        .canRedo = editService_->canRedo(),
        .clean = editService_->isClean(),
        .historyCursor = editService_->historyCursor(),
    };
}

}  // namespace creator::app
