#include "domain/ImportRecordingCommand.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace creator::domain {
namespace {

core::AppError alreadyExists(std::string message) {
    return {core::ErrorCode::AlreadyExists, std::move(message)};
}

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidArgument, std::move(message)};
}

core::AppError invalidState(std::string message) {
    return {core::ErrorCode::InvalidState, std::move(message)};
}

}  // namespace

core::Result<void> ImportRecordingCommand::validateImport(
    const std::vector<Track>& tracks,
    const std::vector<TimelineMarker>& markers) {
    if (tracks.empty() && markers.empty()) {
        return invalid("recording import must contain a track or marker");
    }

    std::set<TrackId> trackIds;
    std::set<ClipId> clipIds;
    std::set<CueId> cueIds;
    for (const auto& track : tracks) {
        if (track.kind() != TrackKind::Video && track.kind() != TrackKind::Audio) {
            return invalid("recording import accepts only video and audio tracks");
        }
        if (!trackIds.insert(track.id()).second) {
            return alreadyExists("recording import track id already exists");
        }
        if (!isValidUtf8(track.id().value()) || !isValidUtf8(track.name())) {
            return invalid("recording import track text is not valid UTF-8");
        }
        if (auto valid = Timeline::validateClips(track.kind(), track.clips());
            !valid.hasValue()) {
            return valid.error();
        }
        for (const auto& clip : track.clips()) {
            if (clip.kind() != ClipKind::Asset) {
                return invalid("recording import accepts only asset clips");
            }
            if (!clipIds.insert(clip.id()).second) {
                return alreadyExists("recording import clip id already exists");
            }
            if (!isValidUtf8(clip.id().value()) || !clip.assetId().has_value() ||
                !isValidUtf8(clip.assetId()->value())) {
                return invalid("recording import clip text is not valid UTF-8");
            }
            for (const auto& cue : clip.captionCues()) {
                if (!cueIds.insert(cue.id()).second) {
                    return alreadyExists("recording import cue id already exists");
                }
            }
        }
    }

    std::set<MarkerId> markerIds;
    std::set<core::TimestampNs> markerPositions;
    for (const auto& marker : markers) {
        if (!markerIds.insert(marker.id()).second) {
            return alreadyExists("recording import marker id already exists");
        }
        if (!markerPositions.insert(marker.position()).second) {
            return alreadyExists("recording import marker position already exists");
        }
        if (!isValidUtf8(marker.id().value()) ||
            !isValidUtf8(marker.label())) {
            return invalid("recording import marker text is not valid UTF-8");
        }
    }
    return core::ok();
}

core::Result<std::unique_ptr<ImportRecordingCommand>>
ImportRecordingCommand::create(CommandId commandId, std::vector<Track> tracks,
                               std::vector<TimelineMarker> markers) {
    if (auto valid = validateImport(tracks, markers); !valid.hasValue()) {
        return valid.error();
    }
    return std::unique_ptr<ImportRecordingCommand>{new ImportRecordingCommand{
        std::move(commandId), std::move(tracks), std::move(markers)}};
}

core::Result<void> ImportRecordingCommand::execute(Timeline& timeline) {
    if (applied_) {
        return invalidState("recording import command is already applied");
    }

    Timeline staged = timeline;
    for (const auto& track : tracks_) {
        if (staged.track(track.id()) != nullptr) {
            return alreadyExists("recording import track collides with timeline");
        }
        for (const auto& clip : track.clips()) {
            if (staged.containsClipId(clip.id())) {
                return alreadyExists("recording import clip collides with timeline");
            }
            for (const auto& cue : clip.captionCues()) {
                if (staged.containsCueId(cue.id())) {
                    return alreadyExists("recording import cue collides with timeline");
                }
            }
        }
    }
    for (const auto& marker : markers_) {
        if (staged.marker(marker.id()) != nullptr) {
            return alreadyExists("recording import marker collides with timeline");
        }
        const auto samePosition = std::ranges::find(
            staged.markers(), marker.position(), &TimelineMarker::position);
        if (samePosition != staged.markers().end()) {
            return alreadyExists(
                "recording import marker position collides with timeline");
        }
    }

    for (const auto& track : tracks_) {
        if (auto added = staged.addTrack(track); !added.hasValue()) {
            return added.error();
        }
    }
    for (const auto& marker : markers_) {
        if (auto added = staged.addMarker(marker); !added.hasValue()) {
            return added.error();
        }
    }
    timeline = std::move(staged);
    applied_ = true;
    return core::ok();
}

core::Result<void> ImportRecordingCommand::undo(Timeline& timeline) {
    if (!applied_) {
        return invalidState("recording import command is not applied");
    }

    Timeline staged = timeline;
    for (const auto& track : tracks_) {
        const auto* current = staged.track(track.id());
        if (current == nullptr || *current != track) {
            return invalidState("imported recording track changed before undo");
        }
    }
    for (const auto& marker : markers_) {
        const auto* current = staged.marker(marker.id());
        if (current == nullptr || *current != marker) {
            return invalidState("imported recording marker changed before undo");
        }
    }

    for (const auto& marker : markers_) {
        if (auto removed = staged.removeMarker(marker.id()); !removed.hasValue()) {
            return removed.error();
        }
    }
    for (const auto& track : tracks_) {
        const auto found = std::ranges::find(staged.tracks_, track.id(), &Track::id);
        if (found == staged.tracks_.end()) {
            return invalidState("imported recording track disappeared during undo");
        }
        staged.tracks_.erase(found);
    }
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord ImportRecordingCommand::record() const {
    const auto payload =
        "{\"markers\":" + internal::serializeTimelineMarkers(markers_) +
        ",\"tracks\":" + internal::serializeTracks(tracks_) +
        ",\"version\":1}";
    return {commandId_, "IMPORT_RECORDING", payload, "{}"};
}

std::unique_ptr<IEditCommand> ImportRecordingCommand::clone() const {
    return std::unique_ptr<IEditCommand>{new ImportRecordingCommand(*this)};
}

core::Result<std::unique_ptr<IEditCommand>> ImportRecordingCommand::rehydrate(
    CommandId commandId, std::vector<Track> tracks,
    std::vector<TimelineMarker> markers, bool applied) {
    auto created = create(std::move(commandId), std::move(tracks),
                          std::move(markers));
    if (!created.hasValue()) return created.error();
    created.value()->applied_ = applied;
    return std::unique_ptr<IEditCommand>{std::move(created).value()};
}

}  // namespace creator::domain
