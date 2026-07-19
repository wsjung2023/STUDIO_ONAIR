#pragma once

#include "domain/EditCommand.h"
#include "domain/Timeline.h"

#include <memory>
#include <vector>

namespace creator::domain {

class ImportRecordingCommand final : public IEditCommand {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<ImportRecordingCommand>>
    create(CommandId commandId, std::vector<Track> tracks,
           std::vector<TimelineMarker> markers);

    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static core::Result<std::unique_ptr<IEditCommand>> rehydrate(
        CommandId commandId, std::vector<Track> tracks,
        std::vector<TimelineMarker> markers, bool applied);

private:
    [[nodiscard]] static core::Result<void> validateImport(
        const std::vector<Track>& tracks,
        const std::vector<TimelineMarker>& markers);

    ImportRecordingCommand(CommandId commandId, std::vector<Track> tracks,
                           std::vector<TimelineMarker> markers)
        : commandId_(std::move(commandId)), tracks_(std::move(tracks)),
          markers_(std::move(markers)) {}

    CommandId commandId_;
    std::vector<Track> tracks_;
    std::vector<TimelineMarker> markers_;
    bool applied_{false};
};

}  // namespace creator::domain
