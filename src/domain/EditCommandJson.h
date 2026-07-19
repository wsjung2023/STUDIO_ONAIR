#pragma once

#include "domain/Identifiers.h"
#include "domain/Timeline.h"

#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace creator::domain::internal {

using TrackClips = std::pair<TrackId, std::vector<Clip>>;

[[nodiscard]] std::string serializeJsonString(std::string_view value);
[[nodiscard]] std::string serializeVisualTransform(
    const std::optional<VisualTransform>& visual);
[[nodiscard]] std::string serializeAudioEnvelope(
    const std::optional<AudioEnvelope>& audio);
[[nodiscard]] std::string serializeTitlePayload(const TitlePayload& title);
[[nodiscard]] std::string serializeCaptionCue(const CaptionCue& cue);
[[nodiscard]] std::string serializeGeneratedClip(const Clip& clip);
[[nodiscard]] std::string serializeClip(const Clip& clip);
[[nodiscard]] std::string serializeTrack(const Track& track);
[[nodiscard]] std::string serializeTimelineMarker(const TimelineMarker& marker);
[[nodiscard]] std::string serializeTracks(const std::vector<Track>& tracks);
[[nodiscard]] std::string serializeTimelineMarkers(
    const std::vector<TimelineMarker>& markers);
[[nodiscard]] std::string serializeTrackClips(
    const std::vector<TrackClips>& tracks);

}  // namespace creator::domain::internal
