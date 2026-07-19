#pragma once

#include "core/Timebase.h"
#include "domain/EditCommand.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"

#include <memory>
#include <optional>

namespace creator::domain {

enum class TrimEdge { Leading, Trailing };

class TrimClipCommand final : public IEditCommand {
public:
    TrimClipCommand(CommandId commandId, TrackId trackId, ClipId clipId,
                    TrimEdge edge, core::TimestampNs boundary)
        : commandId_(std::move(commandId)),
          trackId_(std::move(trackId)),
          clipId_(std::move(clipId)),
          edge_(edge),
          boundary_(boundary) {}

    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId, TrimEdge edge,
        core::TimestampNs boundary, Clip original, bool applied);

private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    TrimEdge edge_;
    core::TimestampNs boundary_;
    std::optional<Clip> original_;
    bool applied_{false};
};

}  // namespace creator::domain
