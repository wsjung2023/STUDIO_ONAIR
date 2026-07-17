#pragma once

#include "core/Timebase.h"
#include "core/Result.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "edit_engine/EditEngineTypes.h"

#include <optional>
#include <memory>
#include <cstddef>
#include <string>
#include <vector>

namespace creator::app {

enum class EditorEditKind {
    Split,
    TrimLeading,
    TrimTrailing,
    DeleteRange,
    SetVisualTransform,
    SetAudioEnvelope,
    AddTitle,
    EditTitle,
    RemoveGeneratedClip,
    AddCaptionCue,
    EditCaptionCue,
    RemoveCaptionCue,
    Undo,
    Redo,
    Save,
};

struct CaptionCueDraft final {
    core::DurationNs startOffset{};
    core::DurationNs duration{};
    std::string text;

    friend bool operator==(const CaptionCueDraft&,
                           const CaptionCueDraft&) = default;
};

struct EditorEditRequest final {
    EditorEditKind kind;
    std::optional<domain::TrackId> trackId{};
    std::optional<domain::ClipId> clipId{};
    core::TimestampNs position{};
    std::optional<domain::TimeRange> range{};
    bool ripple{false};
    std::optional<domain::VisualTransform> visualTransform{};
    std::optional<domain::AudioEnvelope> audioEnvelope{};
    std::optional<domain::TitlePayload> titlePayload{};
    std::optional<domain::CueId> cueId{};
    std::optional<CaptionCueDraft> captionCue{};
};

struct EditorSessionState final {
    std::vector<domain::MediaAsset> assets;
    edit_engine::TimelineSnapshot snapshot;
    bool canUndo{false};
    bool canRedo{false};
    bool clean{true};
    std::size_t historyCursor{0};
};

struct EditorSessionUpdate final {
    EditorSessionState state;
    std::optional<edit_engine::TimelineChangeSet> change{};
    std::optional<core::AppError> derivedWorkDiagnostic{};
};

using EditorSessionResult = core::Result<EditorSessionUpdate>;
using EditorSessionResultPtr = std::shared_ptr<const EditorSessionResult>;

}  // namespace creator::app
