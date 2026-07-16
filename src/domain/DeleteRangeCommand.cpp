#include "domain/DeleteRangeCommand.h"

#include "core/AppError.h"
#include "domain/EditCommandJson.h"

#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

std::string jsonString(std::string_view value) {
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

Result<Clip> resegment(const Clip& clip, ClipId id,
                       core::TimestampNs sourceStart,
                       core::TimestampNs timelineStart,
                       core::DurationNs duration) {
    auto source = TimeRange::create(sourceStart, duration);
    if (!source.hasValue()) return source.error();
    auto placed = TimeRange::create(timelineStart, duration);
    if (!placed.hasValue()) return placed.error();
    return clip.withIdentityAndRanges(std::move(id), source.value(), placed.value());
}

}  // namespace

core::Result<void> DeleteRangeCommand::execute(Timeline& timeline) {
    if (applied_) {
        return AppError{ErrorCode::InvalidState,
                        "delete range command is already applied"};
    }

    const auto deletionStart = deletion_.start();
    const auto deletionEnd = deletion_.end();
    const auto deletionDuration = deletion_.duration();
    std::size_t nextRightId = 0;
    Timeline staged = timeline;
    std::vector<PreviousTrack> previous;

    for (const auto& track : timeline.tracks()) {
        if (track.locked()) continue;
        std::vector<Clip> replacement;
        replacement.reserve(track.clips().size() + 1);

        for (const auto& clip : track.clips()) {
            const auto clipStart = clip.timelineRange().start();
            const auto clipEnd = clip.timelineRange().end();

            if (clipEnd <= deletionStart) {
                replacement.push_back(clip);
                continue;
            }
            if (clipStart >= deletionEnd) {
                if (!ripple_) {
                    replacement.push_back(clip);
                    continue;
                }
                auto shifted = resegment(
                    clip, clip.id(), clip.sourceRange().start(),
                    clipStart - deletionDuration, clip.timelineRange().duration());
                if (!shifted.hasValue()) return shifted.error();
                replacement.push_back(std::move(shifted).value());
                continue;
            }

            if (clipStart >= deletionStart && clipEnd <= deletionEnd) {
                continue;
            }

            if (clipStart < deletionStart && clipEnd > deletionEnd) {
                if (nextRightId >= rightClipIds_.size()) {
                    return AppError{ErrorCode::InvalidArgument,
                                    "delete range needs an identity for each split clip"};
                }
                const auto leftDuration = deletionStart - clipStart;
                auto left = resegment(clip, clip.id(), clip.sourceRange().start(),
                                      clipStart, leftDuration);
                if (!left.hasValue()) return left.error();
                replacement.push_back(std::move(left).value());

                const auto rightDuration = clipEnd - deletionEnd;
                const auto removedSource = deletionEnd - clipStart;
                const auto rightTimelineStart = ripple_ ? deletionStart : deletionEnd;
                auto right = resegment(
                    clip, rightClipIds_[nextRightId++],
                    clip.sourceRange().start() + removedSource,
                    rightTimelineStart, rightDuration);
                if (!right.hasValue()) return right.error();
                replacement.push_back(std::move(right).value());
                continue;
            }

            if (clipStart < deletionStart) {
                const auto leftDuration = deletionStart - clipStart;
                auto left = resegment(clip, clip.id(), clip.sourceRange().start(),
                                      clipStart, leftDuration);
                if (!left.hasValue()) return left.error();
                replacement.push_back(std::move(left).value());
                continue;
            }

            const auto removedSource = deletionEnd - clipStart;
            const auto rightDuration = clipEnd - deletionEnd;
            const auto rightTimelineStart = ripple_ ? deletionStart : deletionEnd;
            auto right = resegment(
                clip, clip.id(), clip.sourceRange().start() + removedSource,
                rightTimelineStart, rightDuration);
            if (!right.hasValue()) return right.error();
            replacement.push_back(std::move(right).value());
        }

        if (replacement != track.clips()) {
            previous.emplace_back(track.id(), track.clips());
            if (auto replaced = staged.replaceTrackClips(track.id(), std::move(replacement));
                !replaced.hasValue()) {
                return replaced.error();
            }
        }
    }

    if (nextRightId != rightClipIds_.size()) {
        return AppError{ErrorCode::InvalidArgument,
                        "delete range received unused split clip identities"};
    }
    previousTracks_ = std::move(previous);
    timeline = std::move(staged);
    applied_ = true;
    return core::ok();
}

core::Result<void> DeleteRangeCommand::undo(Timeline& timeline) {
    if (!applied_) {
        return AppError{ErrorCode::InvalidState,
                        "delete range command is not applied"};
    }
    Timeline staged = timeline;
    for (const auto& [trackId, clips] : previousTracks_) {
        if (auto restored = staged.replaceTrackClips(trackId, clips);
            !restored.hasValue()) {
            return restored.error();
        }
    }
    timeline = std::move(staged);
    applied_ = false;
    return core::ok();
}

EditCommandRecord DeleteRangeCommand::record() const {
    std::string rightIds{"["};
    for (std::size_t index = 0; index < rightClipIds_.size(); ++index) {
        if (index != 0) rightIds.push_back(',');
        rightIds += jsonString(rightClipIds_[index].value());
    }
    rightIds.push_back(']');

    const std::string payload =
        "{\"durationNs\":" + std::to_string(deletion_.duration().count()) +
        ",\"rightClipIds\":" + rightIds +
        ",\"ripple\":" + (ripple_ ? "true" : "false") +
        ",\"startNs\":" +
        std::to_string(deletion_.start().time_since_epoch().count()) + "}";
    return EditCommandRecord{.commandId = commandId_,
                             .type = "DELETE_RANGE",
                             .payload = payload,
                             .undoPayload =
                                 internal::serializeTrackClips(previousTracks_)};
}

std::unique_ptr<IEditCommand> DeleteRangeCommand::clone() const {
    return std::make_unique<DeleteRangeCommand>(*this);
}

std::unique_ptr<IEditCommand> DeleteRangeCommand::rehydrate(
    CommandId commandId, TimeRange deletion, bool ripple,
    std::vector<ClipId> rightClipIds,
    std::vector<PreviousTrack> previousTracks, bool applied) {
    auto command = std::make_unique<DeleteRangeCommand>(
        std::move(commandId), deletion, ripple, std::move(rightClipIds));
    command->previousTracks_ = std::move(previousTracks);
    command->applied_ = applied;
    return command;
}

}  // namespace creator::domain
