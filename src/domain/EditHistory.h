#pragma once

#include "core/Result.h"
#include "domain/EditCommand.h"
#include "domain/Timeline.h"

#include <cstddef>
#include <memory>
#include <optional>
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

    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    void markClean() noexcept { cleanCursor_ = cursor_; }
    [[nodiscard]] bool isClean() const noexcept {
        return cleanCursor_.has_value() && *cleanCursor_ == cursor_;
    }

private:
    void swap(EditHistory& other) noexcept;

    std::size_t limit_;
    std::vector<std::unique_ptr<IEditCommand>> commands_;
    std::size_t cursor_{0};
    std::optional<std::size_t> cleanCursor_{0};
};

}  // namespace creator::domain
