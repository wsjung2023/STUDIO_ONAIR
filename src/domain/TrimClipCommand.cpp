#include "domain/TrimClipCommand.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;

std::string jsonQuoted(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "\\u00" << std::hex << std::setw(2)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

}  // namespace

core::Result<void> TrimClipCommand::execute(Timeline& timeline) {
    if (applied_) {
        return AppError{ErrorCode::InvalidState, "trim command is already applied"};
    }
    const auto* current = timeline.clip(trackId_, clipId_);
    if (current == nullptr) {
        return AppError{ErrorCode::NotFound, "trim clip was not found"};
    }
    const auto start = current->timelineRange().start();
    const auto end = current->timelineRange().end();
    if (boundary_ <= start || boundary_ >= end) {
        return AppError{ErrorCode::InvalidArgument,
                        "trim boundary must be inside the clip"};
    }

    core::TimestampNs sourceStart = current->sourceRange().start();
    core::TimestampNs timelineStart = start;
    core::DurationNs duration{};
    if (edge_ == TrimEdge::Leading) {
        const auto removed = boundary_ - start;
        sourceStart += removed;
        timelineStart = boundary_;
        duration = end - boundary_;
    } else {
        duration = boundary_ - start;
    }
    auto sourceRange = TimeRange::create(sourceStart, duration);
    auto timelineRange = TimeRange::create(timelineStart, duration);
    if (!sourceRange.hasValue()) return sourceRange.error();
    if (!timelineRange.hasValue()) return timelineRange.error();
    auto replacement = current->withIdentityAndRanges(
        clipId_, sourceRange.value(), timelineRange.value());
    if (!replacement.hasValue()) return replacement.error();

    original_ = *current;
    if (auto replaced = timeline.replaceClip(trackId_, clipId_,
                                             std::move(replacement).value());
        !replaced.hasValue()) {
        original_.reset();
        return replaced.error();
    }
    applied_ = true;
    return core::ok();
}

core::Result<void> TrimClipCommand::undo(Timeline& timeline) {
    if (!applied_ || !original_.has_value()) {
        return AppError{ErrorCode::InvalidState, "trim command is not applied"};
    }
    if (auto restored = timeline.replaceClip(trackId_, clipId_, *original_);
        !restored.hasValue()) {
        return restored.error();
    }
    applied_ = false;
    return core::ok();
}

EditCommandRecord TrimClipCommand::record() const {
    const std::string payload =
        "{\"boundaryNs\":" + std::to_string(boundary_.time_since_epoch().count()) +
        ",\"clipId\":" + jsonQuoted(clipId_.value()) +
        ",\"edge\":" + jsonQuoted(edge_ == TrimEdge::Leading ? "LEADING" : "TRAILING") +
        ",\"trackId\":" + jsonQuoted(trackId_.value()) + "}";
    return EditCommandRecord{.commandId = commandId_,
                             .type = "TRIM_CLIP",
                             .payload = payload,
                             .undoPayload = original_.has_value()
                                                ? internal::serializeClip(*original_)
                                                : "{}"};
}

std::unique_ptr<IEditCommand> TrimClipCommand::clone() const {
    return std::make_unique<TrimClipCommand>(*this);
}

std::unique_ptr<IEditCommand> TrimClipCommand::rehydrate(
    CommandId commandId, TrackId trackId, ClipId clipId, TrimEdge edge,
    core::TimestampNs boundary, Clip original, bool applied) {
    auto command = std::make_unique<TrimClipCommand>(
        std::move(commandId), std::move(trackId), std::move(clipId), edge,
        boundary);
    command->original_ = std::move(original);
    command->applied_ = applied;
    return command;
}

}  // namespace creator::domain
