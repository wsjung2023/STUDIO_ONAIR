#pragma once

#include "domain/EditCommand.h"
#include "domain/Timeline.h"

#include <memory>
#include <optional>
#include <utility>

namespace creator::domain {

class SetVisualTransformCommand final : public IEditCommand {
public:
    SetVisualTransformCommand(CommandId commandId, TrackId trackId,
                              ClipId clipId,
                              std::optional<VisualTransform> value)
        : commandId_(std::move(commandId)),
          trackId_(std::move(trackId)),
          clipId_(std::move(clipId)),
          value_(std::move(value)) {}

    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId,
        std::optional<VisualTransform> value,
        std::optional<VisualTransform> previous, bool applied);

private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    std::optional<VisualTransform> value_;
    std::optional<VisualTransform> previous_;
    bool applied_{false};
};

}  // namespace creator::domain
