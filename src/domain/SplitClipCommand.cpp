#include "domain/SplitClipCommand.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace creator::domain {

using core::AppError;
using core::ErrorCode;

core::Result<void> SplitClipCommand::execute(Timeline& timeline) {
    if (applied_) {
        return AppError{ErrorCode::InvalidState, "split command is already applied"};
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) {
        return AppError{ErrorCode::NotFound, "split clip was not found"};
    }
    const auto start = current->timelineRange().start();
    const auto end = current->timelineRange().end();
    if (splitAt_ <= start || splitAt_ >= end) {
        return AppError{ErrorCode::InvalidArgument,
                        "split point must be inside the clip"};
    }

    const auto leftDuration = splitAt_ - start;
    const auto rightDuration = end - splitAt_;
    const auto rightSourceStart = current->sourceRange().start() + leftDuration;
    auto leftSource = TimeRange::create(current->sourceRange().start(), leftDuration);
    auto leftTimeline = TimeRange::create(start, leftDuration);
    auto rightSource = TimeRange::create(rightSourceStart, rightDuration);
    auto rightTimeline = TimeRange::create(splitAt_, rightDuration);
    if (!leftSource.hasValue()) return leftSource.error();
    if (!leftTimeline.hasValue()) return leftTimeline.error();
    if (!rightSource.hasValue()) return rightSource.error();
    if (!rightTimeline.hasValue()) return rightTimeline.error();

    auto left = current->withIdentityAndRanges(clipId_, leftSource.value(),
                                               leftTimeline.value());
    if (!left.hasValue()) return left.error();
    auto right = current->withIdentityAndRanges(rightClipId_, rightSource.value(),
                                                rightTimeline.value());
    if (!right.hasValue()) return right.error();

    Timeline staged = timeline;
    if (auto replaced = staged.replaceClip(trackId_, clipId_, std::move(left).value());
        !replaced.hasValue()) {
        return replaced.error();
    }
    if (auto inserted = staged.insertClip(trackId_, std::move(right).value());
        !inserted.hasValue()) {
        return inserted.error();
    }
    original_ = *current;
    timeline = std::move(staged);
    applied_ = true;
    return core::ok();
}

core::Result<void> SplitClipCommand::undo(Timeline& timeline) {
    if (!applied_ || !original_.has_value()) {
        return AppError{ErrorCode::InvalidState, "split command is not applied"};
    }
    Timeline staged = timeline;
    if (auto removed = staged.removeClip(trackId_, rightClipId_); !removed.hasValue()) {
        return removed.error();
    }
    if (auto restored = staged.replaceClip(trackId_, clipId_, *original_);
        !restored.hasValue()) {
        return restored.error();
    }
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord SplitClipCommand::record() const {
    const auto splitNs = splitAt_.time_since_epoch().count();
    const std::string payload =
        "{\"clipId\":" + internal::serializeJsonString(clipId_.value()) +
        ",\"rightClipId\":" + internal::serializeJsonString(rightClipId_.value()) +
        ",\"splitNs\":" + std::to_string(splitNs) +
        ",\"trackId\":" + internal::serializeJsonString(trackId_.value()) + "}";
    return EditCommandRecord{.commandId = commandId_,
                             .type = "SPLIT_CLIP",
                             .payload = payload,
                             .undoPayload = original_.has_value()
                                                ? internal::serializeClip(*original_)
                                                : "{}"};
}

std::unique_ptr<IEditCommand> SplitClipCommand::clone() const {
    return std::make_unique<SplitClipCommand>(*this);
}

std::unique_ptr<IEditCommand> SplitClipCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId, ClipId rightClipId,
    core::TimestampNs splitAt, Clip original, bool applied) {
    auto command = std::make_unique<SplitClipCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId),
        std::move(rightClipId), splitAt);
    command->original_ = std::move(original);
    command->applied_ = applied;
    return command;
}

}  // namespace creator::domain
