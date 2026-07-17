#include "domain/SetAudioEnvelopeCommand.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <memory>
#include <utility>

namespace creator::domain {

core::Result<void> SetAudioEnvelopeCommand::execute(Timeline& timeline) {
    if (applied_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "audio envelope command is already applied"};
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) {
        return core::AppError{core::ErrorCode::NotFound,
                              "audio envelope clip was not found"};
    }
    auto replacement = current->withAudioEnvelope(value_);
    if (!replacement.hasValue()) return replacement.error();
    previous_ = current->audioEnvelope();
    auto replaced = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!replaced.hasValue()) {
        previous_.reset();
        return replaced.error();
    }
    applied_ = true;
    return core::ok();
}

core::Result<void> SetAudioEnvelopeCommand::undo(Timeline& timeline) {
    if (!applied_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "audio envelope command is not applied"};
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) {
        return core::AppError{core::ErrorCode::NotFound,
                              "audio envelope clip was not found"};
    }
    auto replacement = current->withAudioEnvelope(previous_);
    if (!replacement.hasValue()) return replacement.error();
    auto restored = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!restored.hasValue()) return restored.error();
    applied_ = false;
    return core::ok();
}

EditCommandRecord SetAudioEnvelopeCommand::record() const {
    const auto payload =
        "{\"audio\":" + internal::serializeAudioEnvelope(value_) +
        ",\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"version\":1}";
    const auto undo =
        "{\"previous\":" + internal::serializeAudioEnvelope(previous_) + "}";
    return EditCommandRecord{commandId_, "SET_AUDIO_ENVELOPE", payload, undo};
}

std::unique_ptr<IEditCommand> SetAudioEnvelopeCommand::clone() const {
    return std::make_unique<SetAudioEnvelopeCommand>(*this);
}

std::unique_ptr<IEditCommand> SetAudioEnvelopeCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId,
    std::optional<AudioEnvelope> value,
    std::optional<AudioEnvelope> previous, bool applied) {
    auto command = std::make_unique<SetAudioEnvelopeCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId),
        std::move(value));
    command->previous_ = std::move(previous);
    command->applied_ = applied;
    return command;
}

}  // namespace creator::domain
