#pragma once

#include "core/Timebase.h"
#include "domain/EditCommand.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"

#include <memory>
#include <optional>

namespace creator::domain {

class SplitClipCommand final : public IEditCommand {
public:
    SplitClipCommand(CommandId commandId, TrackId trackId, ClipId clipId,
                     ClipId rightClipId, core::TimestampNs splitAt)
        : commandId_(std::move(commandId)),
          trackId_(std::move(trackId)),
          clipId_(std::move(clipId)),
          rightClipId_(std::move(rightClipId)),
          splitAt_(splitAt) {}

    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId,
        ClipId rightClipId, core::TimestampNs splitAt, Clip original,
        bool applied);

private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    ClipId rightClipId_;
    core::TimestampNs splitAt_;
    std::optional<Clip> original_;
    bool applied_{false};
};

}  // namespace creator::domain
