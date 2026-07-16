#pragma once

#include "core/Result.h"
#include "domain/EditCommand.h"
#include "domain/Timeline.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace creator::domain {

class EditHistory final {
public:
    explicit EditHistory(std::size_t limit);
    EditHistory(const EditHistory& other);
    EditHistory& operator=(const EditHistory& other);
    EditHistory(EditHistory&&) noexcept = default;
    EditHistory& operator=(EditHistory&&) noexcept = default;

    [[nodiscard]] core::Result<void> execute(
        Timeline& timeline, std::unique_ptr<IEditCommand> command);
    [[nodiscard]] core::Result<void> undo(Timeline& timeline);
    [[nodiscard]] core::Result<void> redo(Timeline& timeline);
    [[nodiscard]] static core::Result<EditHistory> restore(
        std::size_t limit,
        std::vector<std::unique_ptr<IEditCommand>> commands,
        std::size_t cursor, std::optional<std::size_t> cleanCursor);

    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::optional<std::size_t> cleanCursor() const noexcept {
        return cleanCursor_;
    }
    [[nodiscard]] std::optional<EditCommandRecord> undoRecord() const;
    [[nodiscard]] std::optional<EditCommandRecord> redoRecord() const;
    void markClean() noexcept { cleanCursor_ = cursor_; }
    [[nodiscard]] bool isClean() const noexcept {
        return cleanCursor_.has_value() && *cleanCursor_ == cursor_;
    }

private:
    EditHistory(std::size_t limit,
                std::vector<std::unique_ptr<IEditCommand>> commands,
                std::size_t cursor, std::optional<std::size_t> cleanCursor)
        : limit_(limit),
          commands_(std::move(commands)),
          cursor_(cursor),
          cleanCursor_(cleanCursor) {}

    void swap(EditHistory& other) noexcept;

    std::size_t limit_;
    std::vector<std::unique_ptr<IEditCommand>> commands_;
    std::size_t cursor_{0};
    std::optional<std::size_t> cleanCursor_{0};
};

}  // namespace creator::domain
