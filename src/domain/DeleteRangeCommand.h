#pragma once

#include "domain/EditCommand.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"
#include "domain/TimelineTypes.h"

#include <memory>
#include <utility>
#include <vector>

namespace creator::domain {

class DeleteRangeCommand final : public IEditCommand {
public:
    DeleteRangeCommand(CommandId commandId, TimeRange deletion, bool ripple,
                       std::vector<ClipId> rightClipIds)
        : commandId_(std::move(commandId)),
          deletion_(deletion),
          ripple_(ripple),
          rightClipIds_(std::move(rightClipIds)) {}

    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;

private:
    using PreviousTrack = std::pair<TrackId, std::vector<Clip>>;

    CommandId commandId_;
    TimeRange deletion_;
    bool ripple_;
    std::vector<ClipId> rightClipIds_;
    std::vector<PreviousTrack> previousTracks_;
    bool applied_{false};
};

}  // namespace creator::domain
