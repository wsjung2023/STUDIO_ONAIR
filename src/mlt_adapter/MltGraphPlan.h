#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "domain/TimelineTypes.h"
#include "edit_engine/EditEngineTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace creator::mlt_adapter {

struct MltGraphClip final {
    domain::ClipId id;
    domain::AssetId assetId;
    domain::MediaKind mediaKind;
    std::filesystem::path mediaPath;
    bool available;
    bool enabled;
    std::int64_t sourceIn;
    std::int64_t sourceOut;
    std::int64_t timelineIn;
    std::int64_t timelineOut;
    std::optional<domain::VisualTransform> visualTransform;
    std::optional<domain::AudioEnvelope> audioEnvelope;
};

struct MltGraphTrack final {
    domain::TrackId id;
    domain::TrackKind kind;
    bool enabled;
    std::vector<MltGraphClip> clips;
};

enum class MltVisualSourceKind { Asset, Generated };

struct MltVisualOrderKey final {
    std::int32_t zOrder;
    std::size_t trackPosition;
    std::int64_t timelineStart;
    std::string identity;
};

struct MltVisualBranch final {
    domain::ClipId clipId;
    std::optional<domain::CueId> cueId;
    std::optional<domain::AssetId> assetId;
    MltVisualSourceKind sourceKind;
    std::filesystem::path sourcePath;
    bool available;
    bool enabled;
    std::int64_t sourceIn;
    std::int64_t sourceOut;
    std::int64_t timelineIn;
    std::int64_t timelineOut;
    domain::VisualTransform transform;
    MltVisualOrderKey order;
};

struct MltGraphPlan final {
    core::FrameRate frameRate;
    domain::TimelineRevision revision;
    std::int32_t canvasWidth;
    std::int32_t canvasHeight;
    // Kept until the physical MLT graph builder is switched to the typed
    // branches below. Generated clips are intentionally absent here.
    std::vector<MltGraphTrack> tracks;
    std::vector<MltGraphTrack> audioTracks;
    std::vector<MltVisualBranch> visualBranches;
    std::vector<std::string> diagnostics;
    std::int64_t durationFrames;
};

[[nodiscard]] core::Result<MltGraphPlan> compileMltGraphPlan(
    const edit_engine::TimelineSnapshot& snapshot);

}  // namespace creator::mlt_adapter
