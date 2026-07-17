#pragma once

#include "domain/EditCommand.h"
#include "domain/Timeline.h"

#include <memory>
#include <optional>
#include <utility>

namespace creator::domain {

class SetAudioEnvelopeCommand final : public IEditCommand {
public:
    SetAudioEnvelopeCommand(CommandId commandId, TrackId trackId,
                            ClipId clipId,
                            std::optional<AudioEnvelope> value)
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
        std::optional<AudioEnvelope> value,
        std::optional<AudioEnvelope> previous,
        core::DurationNs clipDuration, bool applied);

private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    std::optional<AudioEnvelope> value_;
    std::optional<AudioEnvelope> previous_;
    core::DurationNs clipDuration_{};
    bool applied_{false};
};

}  // namespace creator::domain
