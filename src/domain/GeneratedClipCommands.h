#pragma once

#include "domain/EditCommand.h"
#include "domain/Timeline.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace creator::domain {

class AddTitleCommand final : public IEditCommand {
public:
    AddTitleCommand(CommandId commandId, TrackId trackId,
                    std::string trackName, Clip titleClip)
        : commandId_(std::move(commandId)),
          trackId_(std::move(trackId)),
          trackName_(std::move(trackName)),
          titleClip_(std::move(titleClip)) {}
    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, std::string trackName,
        Clip titleClip, bool createdTrack, bool applied);
private:
    CommandId commandId_;
    TrackId trackId_;
    std::string trackName_;
    Clip titleClip_;
    bool createdTrack_{false};
    bool applied_{false};
};

class EditTitleCommand final : public IEditCommand {
public:
    EditTitleCommand(CommandId commandId, TrackId trackId, ClipId clipId,
                     TitlePayload value)
        : commandId_(std::move(commandId)), trackId_(std::move(trackId)),
          clipId_(std::move(clipId)), value_(std::move(value)) {}
    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId,
        TitlePayload value, TitlePayload previous, bool applied);
private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    TitlePayload value_;
    std::optional<TitlePayload> previous_;
    bool applied_{false};
};

class RemoveGeneratedClipCommand final : public IEditCommand {
public:
    RemoveGeneratedClipCommand(CommandId commandId, TrackId trackId,
                               ClipId clipId)
        : commandId_(std::move(commandId)), trackId_(std::move(trackId)),
          clipId_(std::move(clipId)) {}
    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId,
        Clip removed, bool applied);
private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    std::optional<Clip> removed_;
    bool applied_{false};
};

class AddCaptionCueCommand final : public IEditCommand {
public:
    AddCaptionCueCommand(CommandId commandId, TrackId trackId,
                         std::string trackName, ClipId clipId,
                         TimeRange clipRange, bool enabled,
                         std::optional<VisualTransform> visual,
                         CaptionCue cue)
        : commandId_(std::move(commandId)), trackId_(std::move(trackId)),
          trackName_(std::move(trackName)), clipId_(std::move(clipId)),
          clipRange_(clipRange), enabled_(enabled), visual_(std::move(visual)),
          cue_(std::move(cue)) {}
    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, std::string trackName,
        ClipId clipId, TimeRange clipRange, bool enabled,
        std::optional<VisualTransform> visual, CaptionCue cue,
        bool createdTrack, bool createdClip, bool applied);
private:
    CommandId commandId_;
    TrackId trackId_;
    std::string trackName_;
    ClipId clipId_;
    TimeRange clipRange_;
    bool enabled_;
    std::optional<VisualTransform> visual_;
    CaptionCue cue_;
    bool createdTrack_{false};
    bool createdClip_{false};
    bool applied_{false};
};

class EditCaptionCueCommand final : public IEditCommand {
public:
    EditCaptionCueCommand(CommandId commandId, TrackId trackId,
                          ClipId clipId, CueId cueId,
                          CaptionCue replacement)
        : commandId_(std::move(commandId)), trackId_(std::move(trackId)),
          clipId_(std::move(clipId)), cueId_(std::move(cueId)),
          replacement_(std::move(replacement)) {}
    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId, CueId cueId,
        CaptionCue replacement, CaptionCue previous, bool applied);
private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    CueId cueId_;
    CaptionCue replacement_;
    std::optional<CaptionCue> previous_;
    bool applied_{false};
};

class RemoveCaptionCueCommand final : public IEditCommand {
public:
    RemoveCaptionCueCommand(CommandId commandId, TrackId trackId,
                            ClipId clipId, CueId cueId)
        : commandId_(std::move(commandId)), trackId_(std::move(trackId)),
          clipId_(std::move(clipId)), cueId_(std::move(cueId)) {}
    [[nodiscard]] core::Result<void> execute(Timeline& timeline) override;
    [[nodiscard]] core::Result<void> undo(Timeline& timeline) override;
    [[nodiscard]] EditCommandRecord record() const override;
    [[nodiscard]] std::unique_ptr<IEditCommand> clone() const override;
    [[nodiscard]] static std::unique_ptr<IEditCommand> rehydrate(
        CommandId commandId, TrackId trackId, ClipId clipId, CueId cueId,
        CaptionCue previous, std::optional<Clip> removedClip, bool applied);
private:
    CommandId commandId_;
    TrackId trackId_;
    ClipId clipId_;
    CueId cueId_;
    std::optional<CaptionCue> previous_;
    std::optional<Clip> removedClip_;
    bool applied_{false};
};

}  // namespace creator::domain
