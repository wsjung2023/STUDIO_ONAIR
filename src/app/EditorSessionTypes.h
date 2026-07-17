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
#include <vector>

namespace creator::app {

enum class EditorEditKind {
    Split,
    TrimLeading,
    TrimTrailing,
    DeleteRange,
    Undo,
    Redo,
    Save,
};

struct EditorEditRequest final {
    EditorEditKind kind;
    std::optional<domain::TrackId> trackId{};
    std::optional<domain::ClipId> clipId{};
    core::TimestampNs position{};
    std::optional<domain::TimeRange> range{};
    bool ripple{false};
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
};

using EditorSessionResult = core::Result<EditorSessionUpdate>;
using EditorSessionResultPtr = std::shared_ptr<const EditorSessionResult>;

}  // namespace creator::app
