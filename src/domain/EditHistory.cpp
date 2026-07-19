#include "domain/EditHistory.h"

#include "core/AppError.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>

namespace creator::domain {

EditHistory::EditHistory(std::size_t limit) : limit_(limit) {
    assert(limit > 0 && "EditHistory limit must be positive");
}

EditHistory::EditHistory(const EditHistory& other)
    : limit_(other.limit_), cursor_(other.cursor_), cleanCursor_(other.cleanCursor_) {
    commands_.reserve(other.commands_.size());
    for (const auto& command : other.commands_) {
        commands_.push_back(command->clone());
    }
}

EditHistory& EditHistory::operator=(const EditHistory& other) {
    if (this == &other) return *this;
    EditHistory copied{other};
    swap(copied);
    return *this;
}

void EditHistory::swap(EditHistory& other) noexcept {
    using std::swap;
    swap(limit_, other.limit_);
    swap(commands_, other.commands_);
    swap(cursor_, other.cursor_);
    swap(cleanCursor_, other.cleanCursor_);
}

core::Result<void> EditHistory::execute(
    Timeline& timeline, std::unique_ptr<IEditCommand> command) {
    if (!command) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "edit command must not be null"};
    }
    if (auto result = command->execute(timeline); !result.hasValue()) {
        return result.error();
    }

    if (cursor_ < commands_.size()) {
        commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                        commands_.end());
        if (cleanCursor_.has_value() && *cleanCursor_ > cursor_) {
            cleanCursor_.reset();
        }
    }
    commands_.push_back(std::move(command));
    cursor_ = commands_.size();
    if (commands_.size() > limit_) {
        commands_.erase(commands_.begin());
        --cursor_;
        if (cleanCursor_.has_value()) {
            if (*cleanCursor_ == 0) {
                cleanCursor_.reset();
            } else {
                --*cleanCursor_;
            }
        }
    }
    return core::ok();
}

core::Result<void> EditHistory::undo(Timeline& timeline) {
    if (cursor_ == 0) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "edit history has nothing to undo"};
    }
    if (auto result = commands_[cursor_ - 1]->undo(timeline); !result.hasValue()) {
        return result.error();
    }
    --cursor_;
    return core::ok();
}

core::Result<void> EditHistory::redo(Timeline& timeline) {
    if (cursor_ >= commands_.size()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "edit history has nothing to redo"};
    }
    if (auto result = commands_[cursor_]->execute(timeline); !result.hasValue()) {
        return result.error();
    }
    ++cursor_;
    return core::ok();
}

core::Result<EditHistory> EditHistory::restore(
    std::size_t limit, std::vector<std::unique_ptr<IEditCommand>> commands,
    std::size_t cursor, std::optional<std::size_t> cleanCursor) {
    if (limit == 0 || commands.size() > limit || cursor > commands.size() ||
        (cleanCursor.has_value() && *cleanCursor > commands.size())) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "restored edit history metadata is invalid"};
    }
    if (std::any_of(commands.begin(), commands.end(),
                    [](const auto& command) { return command == nullptr; })) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "restored edit history contains a null command"};
    }
    return EditHistory{limit, std::move(commands), cursor, cleanCursor};
}

std::optional<EditCommandRecord> EditHistory::undoRecord() const {
    if (cursor_ == 0) return std::nullopt;
    return commands_[cursor_ - 1]->record();
}

std::optional<EditCommandRecord> EditHistory::redoRecord() const {
    if (cursor_ >= commands_.size()) return std::nullopt;
    return commands_[cursor_]->record();
}

}  // namespace creator::domain
