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

struct MltGraphPlan final {
    core::FrameRate frameRate;
    domain::TimelineRevision revision;
    std::vector<MltGraphTrack> tracks;
    std::int64_t durationFrames;
};

[[nodiscard]] core::Result<MltGraphPlan> compileMltGraphPlan(
    const edit_engine::TimelineSnapshot& snapshot);

}  // namespace creator::mlt_adapter
