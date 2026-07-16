#pragma once

#include "domain/Identifiers.h"
#include "domain/Timeline.h"

#include <string>
#include <utility>
#include <vector>

namespace creator::domain::internal {

using TrackClips = std::pair<TrackId, std::vector<Clip>>;

[[nodiscard]] std::string serializeClip(const Clip& clip);
[[nodiscard]] std::string serializeTrackClips(
    const std::vector<TrackClips>& tracks);

}  // namespace creator::domain::internal
