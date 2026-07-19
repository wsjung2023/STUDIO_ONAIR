#include "domain/SetVisualTransformCommand.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <memory>
#include <utility>

namespace creator::domain {

core::Result<void> SetVisualTransformCommand::execute(Timeline& timeline) {
    if (applied_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "visual transform command is already applied"};
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) {
        return core::AppError{core::ErrorCode::NotFound,
                              "visual transform clip was not found"};
    }
    auto replacement = current->withVisualTransform(value_);
    if (!replacement.hasValue()) return replacement.error();
    previous_ = current->visualTransform();
    auto replaced = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!replaced.hasValue()) {
        previous_.reset();
        return replaced.error();
    }
    applied_ = true;
    return core::ok();
}

core::Result<void> SetVisualTransformCommand::undo(Timeline& timeline) {
    if (!applied_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "visual transform command is not applied"};
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) {
        return core::AppError{core::ErrorCode::NotFound,
                              "visual transform clip was not found"};
    }
    auto replacement = current->withVisualTransform(previous_);
    if (!replacement.hasValue()) return replacement.error();
    auto restored = timeline.replaceClip(
        trackId_, clipId_, std::move(replacement).value());
    if (!restored.hasValue()) return restored.error();
    applied_ = false;
    return core::ok();
}

EditCommandRecord SetVisualTransformCommand::record() const {
    const auto payload =
        "{\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) +
        ",\"version\":1,\"visual\":" +
        internal::serializeVisualTransform(value_) + "}";
    const auto undo =
        "{\"previous\":" + internal::serializeVisualTransform(previous_) + "}";
    return EditCommandRecord{commandId_, "SET_VISUAL_TRANSFORM", payload, undo};
}

std::unique_ptr<IEditCommand> SetVisualTransformCommand::clone() const {
    return std::make_unique<SetVisualTransformCommand>(*this);
}

std::unique_ptr<IEditCommand> SetVisualTransformCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId,
    std::optional<VisualTransform> value,
    std::optional<VisualTransform> previous, bool applied) {
    auto command = std::make_unique<SetVisualTransformCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId),
        std::move(value));
    command->previous_ = std::move(previous);
    command->applied_ = applied;
    return command;
}

}  // namespace creator::domain
