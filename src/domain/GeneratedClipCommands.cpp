#include "domain/GeneratedClipCommands.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace creator::domain {
namespace {

core::AppError invalid(std::string message) {
    return core::AppError{core::ErrorCode::InvalidArgument, std::move(message)};
}

core::AppError invalidState(std::string message) {
    return core::AppError{core::ErrorCode::InvalidState, std::move(message)};
}

core::AppError notFound(std::string message) {
    return core::AppError{core::ErrorCode::NotFound, std::move(message)};
}

bool stableTrack(const TrackId& id, TrackKind kind) noexcept {
    return (kind == TrackKind::Title && id.value() == "title-1") ||
           (kind == TrackKind::Caption && id.value() == "caption-1");
}

std::string boolJson(bool value) {
    return value ? "true" : "false";
}

}  // namespace

core::Result<void> AddTitleCommand::execute(Timeline& timeline) {
    if (applied_) return invalidState("add title command is already applied");
    if (!stableTrack(trackId_, TrackKind::Title) ||
        titleClip_.kind() != ClipKind::Title) {
        return invalid("title command requires the stable title track and a title clip");
    }
    Timeline staged = timeline;
    const auto* existing = staged.track(trackId_);
    bool createdTrack = false;
    if (existing == nullptr) {
        auto track = Track::create(trackId_, TrackKind::Title, trackName_, true, false);
        if (!track.hasValue()) return track.error();
        auto added = staged.addTrack(std::move(track).value());
        if (!added.hasValue()) return added.error();
        createdTrack = true;
    } else if (existing->kind() != TrackKind::Title) {
        return invalid("stable title track has the wrong kind");
    }
    auto inserted = staged.insertClip(trackId_, titleClip_);
    if (!inserted.hasValue()) return inserted.error();
    timeline = std::move(staged);
    createdTrack_ = createdTrack;
    applied_ = true;
    return core::ok();
}

core::Result<void> AddTitleCommand::undo(Timeline& timeline) {
    if (!applied_) return invalidState("add title command is not applied");
    Timeline staged = timeline;
    auto removed = staged.removeClip(trackId_, titleClip_.id());
    if (!removed.hasValue()) return removed.error();
    if (createdTrack_) {
        auto removedTrack = staged.removeTrack(trackId_);
        if (!removedTrack.hasValue()) return removedTrack.error();
    }
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord AddTitleCommand::record() const {
    const auto payload =
        "{\"clip\":" + internal::serializeGeneratedClip(titleClip_) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"trackName\":" + internal::serializeJsonString(trackName_) +
        ",\"version\":1}";
    const auto undo = "{\"createdTrack\":" + boolJson(createdTrack_) + "}";
    return EditCommandRecord{commandId_, "ADD_TITLE", payload, undo};
}

std::unique_ptr<IEditCommand> AddTitleCommand::clone() const {
    return std::make_unique<AddTitleCommand>(*this);
}

std::unique_ptr<IEditCommand> AddTitleCommand::rehydrate(
    CommandId commandId, TrackId trackId, std::string trackName,
    Clip titleClip, bool createdTrack, bool applied) {
    auto command = std::make_unique<AddTitleCommand>(
        std::move(commandId), std::move(trackId), std::move(trackName),
        std::move(titleClip));
    command->createdTrack_ = createdTrack;
    command->applied_ = applied;
    return command;
}

core::Result<void> EditTitleCommand::execute(Timeline& timeline) {
    if (applied_) return invalidState("edit title command is already applied");
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) return notFound("title clip was not found");
    auto replacement = current->withTitlePayload(value_);
    if (!replacement.hasValue()) return replacement.error();
    const auto previous = *current->titlePayload();
    auto replaced = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!replaced.hasValue()) return replaced.error();
    previous_ = previous;
    applied_ = true;
    return core::ok();
}

core::Result<void> EditTitleCommand::undo(Timeline& timeline) {
    if (!applied_ || !previous_.has_value()) {
        return invalidState("edit title command is not applied");
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) return notFound("title clip was not found");
    auto replacement = current->withTitlePayload(*previous_);
    if (!replacement.hasValue()) return replacement.error();
    auto restored = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!restored.hasValue()) return restored.error();
    applied_ = false;
    return core::ok();
}

EditCommandRecord EditTitleCommand::record() const {
    const auto payload =
        "{\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"title\":" + internal::serializeTitlePayload(value_) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"version\":1}";
    const auto undo = previous_.has_value()
                          ? "{\"previous\":" +
                                internal::serializeTitlePayload(*previous_) + "}"
                          : "{}";
    return EditCommandRecord{commandId_, "EDIT_TITLE", payload, undo};
}

std::unique_ptr<IEditCommand> EditTitleCommand::clone() const {
    return std::make_unique<EditTitleCommand>(*this);
}

std::unique_ptr<IEditCommand> EditTitleCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId,
    TitlePayload value, TitlePayload previous, bool applied) {
    auto command = std::make_unique<EditTitleCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId),
        std::move(value));
    command->previous_ = std::move(previous);
    command->applied_ = applied;
    return command;
}

core::Result<void> RemoveGeneratedClipCommand::execute(Timeline& timeline) {
    if (applied_) return invalidState("remove generated clip command is already applied");
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) return notFound("generated clip was not found");
    if (current->kind() == ClipKind::Asset) {
        return invalid("remove generated clip command cannot remove an asset clip");
    }
    Timeline staged = timeline;
    auto removed = staged.removeClip(trackId_, clipId_);
    if (!removed.hasValue()) return removed.error();
    removed_ = std::move(removed).value();
    timeline = std::move(staged);
    applied_ = true;
    return core::ok();
}

core::Result<void> RemoveGeneratedClipCommand::undo(Timeline& timeline) {
    if (!applied_ || !removed_.has_value()) {
        return invalidState("remove generated clip command is not applied");
    }
    Timeline staged = timeline;
    auto inserted = staged.insertClip(trackId_, *removed_);
    if (!inserted.hasValue()) return inserted.error();
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord RemoveGeneratedClipCommand::record() const {
    const auto payload =
        "{\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"version\":1}";
    const auto undo = removed_.has_value()
                          ? "{\"clip\":" + internal::serializeGeneratedClip(*removed_) + "}"
                          : "{}";
    return EditCommandRecord{commandId_, "REMOVE_GENERATED_CLIP", payload, undo};
}

std::unique_ptr<IEditCommand> RemoveGeneratedClipCommand::clone() const {
    return std::make_unique<RemoveGeneratedClipCommand>(*this);
}

std::unique_ptr<IEditCommand> RemoveGeneratedClipCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId,
    Clip removed, bool applied) {
    auto command = std::make_unique<RemoveGeneratedClipCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId));
    command->removed_ = std::move(removed);
    command->applied_ = applied;
    return command;
}

core::Result<void> AddCaptionCueCommand::execute(Timeline& timeline) {
    if (applied_) return invalidState("add caption cue command is already applied");
    if (!stableTrack(trackId_, TrackKind::Caption)) {
        return invalid("caption command requires the stable caption track");
    }
    Timeline staged = timeline;
    const auto* existingTrack = staged.track(trackId_);
    bool createdTrack = false;
    if (existingTrack == nullptr) {
        auto track = Track::create(trackId_, TrackKind::Caption,
                                   trackName_, true, false);
        if (!track.hasValue()) return track.error();
        auto added = staged.addTrack(std::move(track).value());
        if (!added.hasValue()) return added.error();
        createdTrack = true;
    } else if (existingTrack->kind() != TrackKind::Caption) {
        return invalid("stable caption track has the wrong kind");
    }

    const auto* current = staged.clip(trackId_, clipId_);
    bool createdClip = false;
    if (current == nullptr) {
        auto clip = Clip::createCaption(
            clipId_, clipRange_, enabled_, {cue_}, visual_);
        if (!clip.hasValue()) return clip.error();
        auto inserted = staged.insertClip(trackId_, std::move(clip).value());
        if (!inserted.hasValue()) return inserted.error();
        createdClip = true;
    } else {
        if (current->kind() != ClipKind::Caption ||
            current->timelineRange() != clipRange_ ||
            current->enabled() != enabled_ ||
            current->visualTransform() != visual_) {
            return invalid("existing caption clip contradicts add cue request");
        }
        auto cues = current->captionCues();
        cues.push_back(cue_);
        auto replacement = current->withCaptionCues(std::move(cues));
        if (!replacement.hasValue()) return replacement.error();
        auto replaced = staged.replaceClip(
            trackId_, clipId_, std::move(replacement).value());
        if (!replaced.hasValue()) return replaced.error();
    }
    timeline = std::move(staged);
    createdTrack_ = createdTrack;
    createdClip_ = createdClip;
    applied_ = true;
    return core::ok();
}

core::Result<void> AddCaptionCueCommand::undo(Timeline& timeline) {
    if (!applied_) return invalidState("add caption cue command is not applied");
    Timeline staged = timeline;
    if (createdClip_) {
        auto removed = staged.removeClip(trackId_, clipId_);
        if (!removed.hasValue()) return removed.error();
    } else {
        const auto* current = staged.clip(trackId_, clipId_);
        if (current == nullptr) return notFound("caption clip was not found");
        auto cues = current->captionCues();
        const auto found = std::find_if(cues.begin(), cues.end(),
                                        [this](const CaptionCue& candidate) {
                                            return candidate.id() == cue_.id();
                                        });
        if (found == cues.end()) return notFound("caption cue was not found");
        cues.erase(found);
        auto replacement = current->withCaptionCues(std::move(cues));
        if (!replacement.hasValue()) return replacement.error();
        auto replaced = staged.replaceClip(
            trackId_, clipId_, std::move(replacement).value());
        if (!replaced.hasValue()) return replaced.error();
    }
    if (createdTrack_) {
        auto removedTrack = staged.removeTrack(trackId_);
        if (!removedTrack.hasValue()) return removedTrack.error();
    }
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord AddCaptionCueCommand::record() const {
    const auto payload =
        "{\"clipDurationNs\":" + std::to_string(clipRange_.duration().count()) +
        ",\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"clipStartNs\":" +
        std::to_string(clipRange_.start().time_since_epoch().count()) +
        ",\"cue\":" + internal::serializeCaptionCue(cue_) +
        ",\"enabled\":" + boolJson(enabled_) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"trackName\":" + internal::serializeJsonString(trackName_) +
        ",\"version\":1,\"visual\":" +
        internal::serializeVisualTransform(visual_) + "}";
    const auto undo =
        "{\"createdClip\":" + boolJson(createdClip_) +
        ",\"createdTrack\":" + boolJson(createdTrack_) + "}";
    return EditCommandRecord{commandId_, "ADD_CAPTION_CUE", payload, undo};
}

std::unique_ptr<IEditCommand> AddCaptionCueCommand::clone() const {
    return std::make_unique<AddCaptionCueCommand>(*this);
}

std::unique_ptr<IEditCommand> AddCaptionCueCommand::rehydrate(
    CommandId commandId, TrackId trackId, std::string trackName,
    ClipId clipId, TimeRange clipRange, bool enabled,
    std::optional<VisualTransform> visual, CaptionCue cue,
    bool createdTrack, bool createdClip, bool applied) {
    auto command = std::make_unique<AddCaptionCueCommand>(
        std::move(commandId), std::move(trackId), std::move(trackName),
        std::move(clipId), clipRange, enabled, std::move(visual),
        std::move(cue));
    command->createdTrack_ = createdTrack;
    command->createdClip_ = createdClip;
    command->applied_ = applied;
    return command;
}

core::Result<void> EditCaptionCueCommand::execute(Timeline& timeline) {
    if (applied_) return invalidState("edit caption cue command is already applied");
    if (replacement_.id() != cueId_) {
        return invalid("caption cue replacement must preserve identity");
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr || current->kind() != ClipKind::Caption) {
        return notFound("caption clip was not found");
    }
    auto cues = current->captionCues();
    const auto found = std::find_if(cues.begin(), cues.end(),
                                    [this](const CaptionCue& candidate) {
                                        return candidate.id() == cueId_;
                                    });
    if (found == cues.end()) return notFound("caption cue was not found");
    const auto previous = *found;
    *found = replacement_;
    auto replacement = current->withCaptionCues(std::move(cues));
    if (!replacement.hasValue()) return replacement.error();
    auto replaced = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!replaced.hasValue()) return replaced.error();
    previous_ = previous;
    applied_ = true;
    return core::ok();
}

core::Result<void> EditCaptionCueCommand::undo(Timeline& timeline) {
    if (!applied_ || !previous_.has_value()) {
        return invalidState("edit caption cue command is not applied");
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) return notFound("caption clip was not found");
    auto cues = current->captionCues();
    const auto found = std::find_if(cues.begin(), cues.end(),
                                    [this](const CaptionCue& candidate) {
                                        return candidate.id() == cueId_;
                                    });
    if (found == cues.end()) return notFound("caption cue was not found");
    *found = *previous_;
    auto replacement = current->withCaptionCues(std::move(cues));
    if (!replacement.hasValue()) return replacement.error();
    auto restored = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!restored.hasValue()) return restored.error();
    applied_ = false;
    return core::ok();
}

EditCommandRecord EditCaptionCueCommand::record() const {
    const auto payload =
        "{\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"cueId\":" + internal::serializeJsonString(cueId_.value()) +
        ",\"replacement\":" + internal::serializeCaptionCue(replacement_) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"version\":1}";
    const auto undo = previous_.has_value()
                          ? "{\"previous\":" +
                                internal::serializeCaptionCue(*previous_) + "}"
                          : "{}";
    return EditCommandRecord{commandId_, "EDIT_CAPTION_CUE", payload, undo};
}

std::unique_ptr<IEditCommand> EditCaptionCueCommand::clone() const {
    return std::make_unique<EditCaptionCueCommand>(*this);
}

std::unique_ptr<IEditCommand> EditCaptionCueCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId, CueId cueId,
    CaptionCue replacement, CaptionCue previous, bool applied) {
    auto command = std::make_unique<EditCaptionCueCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId),
        std::move(cueId), std::move(replacement));
    command->previous_ = std::move(previous);
    command->applied_ = applied;
    return command;
}

core::Result<void> RemoveCaptionCueCommand::execute(Timeline& timeline) {
    if (applied_) return invalidState("remove caption cue command is already applied");
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr || current->kind() != ClipKind::Caption) {
        return notFound("caption clip was not found");
    }
    const auto found = std::find_if(
        current->captionCues().begin(), current->captionCues().end(),
        [this](const CaptionCue& candidate) { return candidate.id() == cueId_; });
    if (found == current->captionCues().end()) {
        return notFound("caption cue was not found");
    }
    const auto previous = *found;
    Timeline staged = timeline;
    std::optional<Clip> removedClip;
    if (current->captionCues().size() == 1) {
        auto removed = staged.removeClip(trackId_, clipId_);
        if (!removed.hasValue()) return removed.error();
        removedClip = std::move(removed).value();
    } else {
        auto cues = current->captionCues();
        cues.erase(std::find_if(cues.begin(), cues.end(),
                                [this](const CaptionCue& candidate) {
                                    return candidate.id() == cueId_;
                                }));
        auto replacement = current->withCaptionCues(std::move(cues));
        if (!replacement.hasValue()) return replacement.error();
        auto replaced = staged.replaceClip(
            trackId_, clipId_, std::move(replacement).value());
        if (!replaced.hasValue()) return replaced.error();
    }
    timeline = std::move(staged);
    previous_ = previous;
    removedClip_ = std::move(removedClip);
    applied_ = true;
    return core::ok();
}

core::Result<void> RemoveCaptionCueCommand::undo(Timeline& timeline) {
    if (!applied_ || !previous_.has_value()) {
        return invalidState("remove caption cue command is not applied");
    }
    Timeline staged = timeline;
    if (removedClip_.has_value()) {
        auto inserted = staged.insertClip(trackId_, *removedClip_);
        if (!inserted.hasValue()) return inserted.error();
    } else {
        const auto* current = staged.clip(trackId_, clipId_);
        if (current == nullptr) return notFound("caption clip was not found");
        auto cues = current->captionCues();
        cues.push_back(*previous_);
        auto replacement = current->withCaptionCues(std::move(cues));
        if (!replacement.hasValue()) return replacement.error();
        auto restored = staged.replaceClip(
            trackId_, clipId_, std::move(replacement).value());
        if (!restored.hasValue()) return restored.error();
    }
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord RemoveCaptionCueCommand::record() const {
    const auto payload =
        "{\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"cueId\":" + internal::serializeJsonString(cueId_.value()) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"version\":1}";
    const auto previous = previous_.has_value()
                              ? internal::serializeCaptionCue(*previous_)
                              : "null";
    const auto clip = removedClip_.has_value()
                          ? internal::serializeGeneratedClip(*removedClip_)
                          : "null";
    const auto undo = "{\"previous\":" + previous +
                      ",\"removedClip\":" + clip + "}";
    return EditCommandRecord{commandId_, "REMOVE_CAPTION_CUE", payload, undo};
}

std::unique_ptr<IEditCommand> RemoveCaptionCueCommand::clone() const {
    return std::make_unique<RemoveCaptionCueCommand>(*this);
}

std::unique_ptr<IEditCommand> RemoveCaptionCueCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId, CueId cueId,
    CaptionCue previous, std::optional<Clip> removedClip, bool applied) {
    auto command = std::make_unique<RemoveCaptionCueCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId),
        std::move(cueId));
    command->previous_ = std::move(previous);
    command->removedClip_ = std::move(removedClip);
    command->applied_ = applied;
    return command;
}

}  // namespace creator::domain
