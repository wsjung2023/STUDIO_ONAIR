#include "app/RecordingImportPlanner.h"

#include "core/AppError.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::StudioSourceRole;

AppError invalid(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message)};
}

AppError notFound(std::string message) {
    return {ErrorCode::NotFound, std::move(message)};
}

Result<TimestampNs> addTime(TimestampNs base, DurationNs offset) {
    const auto first = base.time_since_epoch().count();
    const auto second = offset.count();
    if (first < 0 || second < 0 ||
        first > std::numeric_limits<std::int64_t>::max() - second) {
        return invalid("recording import time exceeds project range");
    }
    return TimestampNs{DurationNs{first + second}};
}

Result<TimestampNs> segmentEnd(const domain::SegmentInfo& segment) {
    return addTime(segment.startTime, segment.duration);
}

TimestampNs timelineEnd(const domain::Timeline& timeline) {
    TimestampNs result{};
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            result = std::max(result, clip.timelineRange().end());
        }
    }
    return result;
}

std::string_view roleToken(StudioSourceRole role) noexcept {
    switch (role) {
        case StudioSourceRole::Screen:
            return "screen";
        case StudioSourceRole::Camera:
            return "camera";
        case StudioSourceRole::Microphone:
            return "microphone";
        case StudioSourceRole::SystemAudio:
            return "system-audio";
    }
    return "unknown";
}

std::string roleName(StudioSourceRole role) {
    switch (role) {
        case StudioSourceRole::Screen:
            return "Screen";
        case StudioSourceRole::Camera:
            return "Camera";
        case StudioSourceRole::Microphone:
            return "Microphone";
        case StudioSourceRole::SystemAudio:
            return "System Audio";
    }
    return "Recorded Source";
}

bool videoRole(StudioSourceRole role) noexcept {
    return role == StudioSourceRole::Screen ||
           role == StudioSourceRole::Camera;
}

bool validSha256(std::string_view value) noexcept {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

Result<void> validateTextInputs(const RecordingImportRequest& request) {
    if (!domain::isValidUtf8(request.sessionId.value())) {
        return invalid("recording session id is not valid UTF-8");
    }
    for (const auto& source : request.sources) {
        if (!domain::isValidUtf8(source.sourceId.value())) {
            return invalid("recording source id is not valid UTF-8");
        }
    }
    for (const auto& segment : request.segments) {
        if (!domain::isValidUtf8(segment.sourceId.value()) ||
            !domain::isValidUtf8(segment.relativePath)) {
            return invalid("recording segment text is not valid UTF-8");
        }
    }
    for (const auto& marker : request.markers) {
        if (!domain::isValidUtf8(marker.markerId) ||
            !domain::isValidUtf8(marker.label)) {
            return invalid("recording marker text is not valid UTF-8");
        }
    }
    for (const auto& probe : request.probes) {
        if (!domain::isValidUtf8(probe.relativePath) ||
            !domain::isValidUtf8(probe.media.formatName) ||
            !domain::isValidUtf8(probe.media.codecName)) {
            return invalid("recording probe text is not valid UTF-8");
        }
    }
    return core::ok();
}

Result<void> validateSceneSnapshots(const RecordingImportRequest& request) {
    std::map<domain::SourceId, StudioSourceRole> roles;
    for (const auto& source : request.sources) {
        roles.emplace(source.sourceId, source.role);
    }
    std::set<domain::SceneId> sceneIds;
    for (const auto& scene : request.scenes) {
        if (!sceneIds.insert(scene.id()).second) {
            return invalid("recording scene id is duplicated");
        }
        if (!domain::isValidUtf8(scene.id().value()) ||
            !domain::isValidUtf8(scene.name())) {
            return invalid("recording scene text is not valid UTF-8");
        }
        for (const auto& source : scene.sources()) {
            if (!domain::isValidUtf8(source.id().value()) ||
                !domain::isValidUtf8(source.name())) {
                return invalid("recording scene source text is not valid UTF-8");
            }
            const auto recordedRole = roles.find(source.id());
            if (recordedRole != roles.end() &&
                recordedRole->second != source.role()) {
                return invalid("recording scene source role does not match");
            }
        }
    }
    return core::ok();
}

struct SourceState final {
    bool enabled{false};
    std::optional<domain::VisualTransform> transform;
};

const domain::StudioScene* findScene(
    const std::vector<domain::StudioScene>& scenes,
    const domain::SceneId& id) noexcept {
    const auto found = std::find_if(
        scenes.begin(), scenes.end(),
        [&id](const domain::StudioScene& scene) { return scene.id() == id; });
    return found == scenes.end() ? nullptr : &*found;
}

SourceState stateInScene(const domain::StudioScene& scene,
                         const domain::SourceId& sourceId) {
    const auto found = std::find_if(
        scene.sources().begin(), scene.sources().end(),
        [&sourceId](const domain::SceneSource& source) {
            return source.id() == sourceId;
        });
    if (found == scene.sources().end()) return {};
    return SourceState{.enabled = found->enabled(),
                       .transform = found->transform()};
}

const project_store::RecordingSceneEvent* activeEventAt(
    const std::vector<project_store::RecordingSceneEvent>& events,
    TimestampNs position, bool includeEqual = true) noexcept {
    const project_store::RecordingSceneEvent* active = nullptr;
    for (const auto& event : events) {
        if (event.position < position ||
            (includeEqual && event.position == position)) {
            active = &event;
        } else {
            break;
        }
    }
    return active;
}

Result<SourceState> stateAt(
    const RecordingImportRequest& request,
    const std::vector<project_store::RecordingSceneEvent>& events,
    const domain::SourceId& sourceId, TimestampNs position,
    bool includeEqual = true) {
    const auto* event = activeEventAt(events, position, includeEqual);
    if (event == nullptr) {
        return invalid("recording has no active scene at segment position");
    }
    const auto* scene = findScene(request.scenes, event->sceneId);
    if (scene == nullptr) return notFound("recording scene snapshot was not found");
    return stateInScene(*scene, sourceId);
}

std::string idPrefix(const domain::SessionId& sessionId) {
    return "recording/" + sessionId.value();
}

struct PlannedSource final {
    domain::SourceId id;
    StudioSourceRole role;
};

Result<std::vector<PlannedSource>> plannedSources(
    const RecordingImportRequest& request) {
    std::map<domain::SourceId, StudioSourceRole> byId;
    std::set<StudioSourceRole> roles;
    for (const auto& source : request.sources) {
        if (!byId.emplace(source.sourceId, source.role).second ||
            !roles.insert(source.role).second) {
            return invalid("recording source roles must be unique");
        }
    }
    std::set<domain::SourceId> observed;
    for (const auto& segment : request.segments) {
        if (segment.status == domain::SegmentStatus::Writing) {
            return invalid("completed recording contains a writing segment");
        }
        observed.insert(segment.sourceId);
    }
    std::vector<PlannedSource> result;
    for (const auto& sourceId : observed) {
        const auto role = byId.find(sourceId);
        if (role == byId.end()) {
            return notFound("recording segment source role was not found");
        }
        result.push_back(PlannedSource{sourceId, role->second});
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        if (first.role != second.role) return first.role < second.role;
        return first.id < second.id;
    });
    return result;
}

Result<std::vector<project_store::RecordingSceneEvent>> validatedEvents(
    const RecordingImportRequest& request) {
    auto result = request.sceneEvents;
    std::set<std::uint64_t> sequences;
    for (const auto& event : result) {
        if (event.sessionId != request.sessionId ||
            event.position.time_since_epoch() < DurationNs::zero() ||
            !sequences.insert(event.sequence).second) {
            return invalid("recording scene events are invalid");
        }
        if (findScene(request.scenes, event.sceneId) == nullptr) {
            return notFound("recording scene snapshot was not found");
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        if (first.position != second.position) return first.position < second.position;
        return first.sequence < second.sequence;
    });
    if (result.empty() || result.front().position != TimestampNs{}) {
        return invalid("recording must begin with an active scene event");
    }
    return result;
}

Result<std::map<std::string, media::MediaProbeResult>> indexedProbes(
    const RecordingImportRequest& request) {
    std::map<std::string, media::MediaProbeResult> result;
    for (const auto& probe : request.probes) {
        if (probe.relativePath.empty() ||
            !result.emplace(probe.relativePath, probe.media).second) {
            return invalid("recording media probes must have unique paths");
        }
        if (probe.media.formatName.empty() || probe.media.codecName.empty() ||
            !validSha256(probe.media.sha256)) {
            return invalid("recording media probe identity is invalid");
        }
    }
    return result;
}

Result<domain::MediaAsset> makeAsset(
    const RecordingImportRequest& request, const domain::SegmentInfo& segment,
    StudioSourceRole role, const media::MediaProbeResult& probe) {
    if (segment.startTime.time_since_epoch() < DurationNs::zero() ||
        segment.duration <= DurationNs::zero() || segment.relativePath.empty()) {
        return invalid("recording segment metadata is invalid");
    }
    auto end = segmentEnd(segment);
    if (!end.hasValue()) return end.error();
    if (probe.duration < segment.duration) {
        return invalid("media probe duration is shorter than its segment");
    }
    if ((videoRole(role) && !probe.video.has_value()) ||
        (!videoRole(role) &&
         (!probe.audio.has_value() || probe.video.has_value()))) {
        return invalid("media probe kind does not match recording source role");
    }
    const auto assetId = domain::AssetId::create(
        idPrefix(request.sessionId) + "/asset/" + segment.sourceId.value() +
        "/" + std::to_string(segment.index));
    if (!assetId.hasValue()) return assetId.error();
    return domain::MediaAsset::create(
        assetId.value(), videoRole(role) ? domain::MediaKind::Video
                                        : domain::MediaKind::Audio,
        segment.relativePath, probe.duration, probe.video, probe.audio,
        probe.byteSize, probe.sha256, domain::AssetAvailability::Available);
}

Result<domain::TrackId> makeTrackId(const RecordingImportRequest& request,
                                    const PlannedSource& source) {
    return domain::TrackId::create(
        idPrefix(request.sessionId) + "/track/" +
        std::string(roleToken(source.role)) + "/" + source.id.value());
}

Result<domain::ClipId> makeClipId(const RecordingImportRequest& request,
                                  const domain::SegmentInfo& segment,
                                  std::size_t piece) {
    return domain::ClipId::create(
        idPrefix(request.sessionId) + "/clip/" + segment.sourceId.value() +
        "/" + std::to_string(segment.index) + "/" + std::to_string(piece));
}

Result<std::vector<TimestampNs>> splitBoundaries(
    const RecordingImportRequest& request,
    const std::vector<project_store::RecordingSceneEvent>& events,
    const domain::SegmentInfo& segment, StudioSourceRole role) {
    auto end = segmentEnd(segment);
    if (!end.hasValue()) return end.error();
    std::vector<TimestampNs> boundaries{segment.startTime};
    for (const auto& event : events) {
        if (event.position <= segment.startTime || event.position >= end.value()) {
            continue;
        }
        if (!videoRole(role)) {
            auto before = stateAt(request, events, segment.sourceId,
                                  event.position, false);
            auto after = stateAt(request, events, segment.sourceId,
                                 event.position, true);
            if (!before.hasValue()) return before.error();
            if (!after.hasValue()) return after.error();
            if (before.value().enabled == after.value().enabled) continue;
        }
        if (boundaries.back() != event.position) boundaries.push_back(event.position);
    }
    boundaries.push_back(end.value());
    return boundaries;
}

Result<void> rejectTimelineCollisions(
    const domain::Timeline& timeline,
    const std::vector<domain::MediaAsset>& assets,
    const std::vector<domain::Track>& tracks,
    const std::vector<domain::TimelineMarker>& markers) {
    std::set<domain::AssetId> existingAssetIds;
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            if (clip.assetId().has_value()) {
                existingAssetIds.insert(*clip.assetId());
            }
        }
    }
    for (const auto& asset : assets) {
        if (existingAssetIds.contains(asset.id())) {
            return AppError{ErrorCode::AlreadyExists,
                            "recording import asset id already exists"};
        }
    }
    for (const auto& track : tracks) {
        if (timeline.track(track.id()) != nullptr) {
            return AppError{ErrorCode::AlreadyExists,
                            "recording import track id already exists"};
        }
        for (const auto& existingTrack : timeline.tracks()) {
            for (const auto& clip : track.clips()) {
                if (timeline.clip(existingTrack.id(), clip.id()) != nullptr) {
                    return AppError{ErrorCode::AlreadyExists,
                                    "recording import clip id already exists"};
                }
            }
        }
    }
    for (const auto& marker : markers) {
        if (timeline.marker(marker.id()) != nullptr) {
            return AppError{ErrorCode::AlreadyExists,
                            "recording import marker id already exists"};
        }
        const auto samePosition = std::find_if(
            timeline.markers().begin(), timeline.markers().end(),
            [&marker](const domain::TimelineMarker& existing) {
                return existing.position() == marker.position();
            });
        if (samePosition != timeline.markers().end()) {
            return AppError{ErrorCode::AlreadyExists,
                            "recording import marker position already exists"};
        }
    }
    return core::ok();
}

}  // namespace

Result<RecordingImportPlan> planRecordingImport(
    const RecordingImportRequest& request) {
    if (auto text = validateTextInputs(request); !text.hasValue()) {
        return text.error();
    }
    auto sources = plannedSources(request);
    if (!sources.hasValue()) return sources.error();
    if (auto scenes = validateSceneSnapshots(request); !scenes.hasValue()) {
        return scenes.error();
    }
    auto events = validatedEvents(request);
    if (!events.hasValue()) return events.error();
    auto probes = indexedProbes(request);
    if (!probes.hasValue()) return probes.error();
    const TimestampNs appendBase = timelineEnd(request.timeline);

    auto staging = domain::Timeline::create(
        domain::TimelineId::create(idPrefix(request.sessionId) + "/staging")
            .value(),
        "Recording import staging", request.timeline.frameRate());
    if (!staging.hasValue()) return staging.error();

    std::map<domain::SourceId, StudioSourceRole> roles;
    std::map<domain::SourceId, domain::TrackId> trackIds;
    for (const auto& source : sources.value()) {
        roles.emplace(source.id, source.role);
        auto trackId = makeTrackId(request, source);
        if (!trackId.hasValue()) return trackId.error();
        trackIds.emplace(source.id, trackId.value());
        auto track = domain::Track::create(
            trackId.value(), videoRole(source.role) ? domain::TrackKind::Video
                                                    : domain::TrackKind::Audio,
            roleName(source.role), true, false);
        if (!track.hasValue()) return track.error();
        auto inserted = staging.value().addTrack(std::move(track).value());
        if (!inserted.hasValue()) return inserted.error();
    }

    auto segments = request.segments;
    std::sort(segments.begin(), segments.end(), [&](const auto& first, const auto& second) {
        const auto firstRole = roles.find(first.sourceId);
        const auto secondRole = roles.find(second.sourceId);
        if (firstRole != roles.end() && secondRole != roles.end() &&
            firstRole->second != secondRole->second) {
            return firstRole->second < secondRole->second;
        }
        if (first.sourceId != second.sourceId) return first.sourceId < second.sourceId;
        if (first.startTime != second.startTime) return first.startTime < second.startTime;
        if (first.index != second.index) return first.index < second.index;
        return first.relativePath < second.relativePath;
    });

    std::vector<domain::MediaAsset> assets;
    std::set<std::pair<domain::SourceId, std::uint64_t>> segmentIds;
    for (const auto& segment : segments) {
        if (!segmentIds.emplace(segment.sourceId, segment.index).second) {
            return invalid("recording segment identity is duplicated");
        }
        if (segment.startTime.time_since_epoch() < DurationNs::zero() ||
            segment.duration <= DurationNs::zero() ||
            segment.relativePath.empty()) {
            return invalid("recording segment metadata is invalid");
        }
        if (auto end = segmentEnd(segment); !end.hasValue()) {
            return end.error();
        }
        if (segment.status == domain::SegmentStatus::Failed) continue;
        const auto role = roles.find(segment.sourceId);
        if (role == roles.end()) return notFound("recording source role was not found");
        const auto probe = probes.value().find(segment.relativePath);
        if (probe == probes.value().end()) {
            return notFound("recording media probe was not found");
        }
        auto asset = makeAsset(request, segment, role->second, probe->second);
        if (!asset.hasValue()) return asset.error();
        auto boundaries = splitBoundaries(request, events.value(), segment,
                                          role->second);
        if (!boundaries.hasValue()) return boundaries.error();
        for (std::size_t index = 0; index + 1 < boundaries.value().size(); ++index) {
            const auto pieceStart = boundaries.value()[index];
            const auto duration = boundaries.value()[index + 1] - pieceStart;
            auto state = stateAt(request, events.value(), segment.sourceId,
                                 pieceStart);
            if (!state.hasValue()) return state.error();
            auto timelineStart = addTime(appendBase, pieceStart.time_since_epoch());
            if (!timelineStart.hasValue()) return timelineStart.error();
            auto sourceRange = domain::TimeRange::create(
                TimestampNs{pieceStart - segment.startTime}, duration);
            if (!sourceRange.hasValue()) return sourceRange.error();
            auto timelineRange =
                domain::TimeRange::create(timelineStart.value(), duration);
            if (!timelineRange.hasValue()) return timelineRange.error();
            auto clipId = makeClipId(request, segment, index);
            if (!clipId.hasValue()) return clipId.error();
            std::optional<domain::AudioEnvelope> envelope;
            if (!videoRole(role->second)) {
                auto created = domain::AudioEnvelope::create(
                    0.0, DurationNs::zero(), DurationNs::zero(), duration);
                if (!created.hasValue()) return created.error();
                envelope = created.value();
            }
            auto clip = domain::Clip::createAsset(
                clipId.value(), asset.value(), sourceRange.value(),
                timelineRange.value(), state.value().enabled,
                videoRole(role->second) ? state.value().transform : std::nullopt,
                envelope);
            if (!clip.hasValue()) return clip.error();
            auto inserted = staging.value().insertClip(
                trackIds.at(segment.sourceId), std::move(clip).value());
            if (!inserted.hasValue()) return inserted.error();
        }
        assets.push_back(std::move(asset).value());
    }

    auto recordingMarkers = request.markers;
    std::sort(recordingMarkers.begin(), recordingMarkers.end(),
              [](const auto& first, const auto& second) {
                  if (first.position != second.position) {
                      return first.position < second.position;
                  }
                  return first.markerId < second.markerId;
              });
    std::vector<domain::TimelineMarker> markers;
    std::set<std::string> markerIds;
    std::set<TimestampNs> markerPositions;
    for (const auto& marker : recordingMarkers) {
        if (marker.sessionId != request.sessionId ||
            marker.position.time_since_epoch() < DurationNs::zero() ||
            !markerIds.insert(marker.markerId).second) {
            return invalid("recording marker is invalid or duplicated");
        }
        auto id = domain::MarkerId::create(marker.markerId);
        if (!id.hasValue()) return id.error();
        auto position = addTime(appendBase, marker.position.time_since_epoch());
        if (!position.hasValue()) return position.error();
        if (!markerPositions.insert(position.value()).second) {
            return AppError{ErrorCode::AlreadyExists,
                            "recording marker position is duplicated"};
        }
        auto created = domain::TimelineMarker::create(
            id.value(), position.value(), marker.label);
        if (!created.hasValue()) return created.error();
        markers.push_back(std::move(created).value());
    }

    auto collision = rejectTimelineCollisions(request.timeline, assets,
                                               staging.value().tracks(), markers);
    if (!collision.hasValue()) return collision.error();
    return RecordingImportPlan{.sessionId = request.sessionId,
                               .appendBase = appendBase,
                               .assets = std::move(assets),
                               .tracks = staging.value().tracks(),
                               .markers = std::move(markers)};
}

}  // namespace creator::app
