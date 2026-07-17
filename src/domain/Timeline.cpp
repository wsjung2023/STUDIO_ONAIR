#include "domain/Timeline.h"

#include "core/AppError.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;

bool compatible(TrackKind trackKind, const Clip& clip) noexcept {
    if (clip.kind() == ClipKind::Title) return trackKind == TrackKind::Title;
    if (clip.kind() == ClipKind::Caption) return trackKind == TrackKind::Caption;
    if (clip.mediaKind() == MediaKind::Audio) return trackKind == TrackKind::Audio;
    return trackKind == TrackKind::Video;
}

core::Result<std::vector<CaptionCue>> validatedCaptionCues(
    std::vector<CaptionCue> cues, core::DurationNs clipDuration) {
    if (cues.empty()) {
        return AppError{ErrorCode::InvalidArgument,
                        "caption clip must contain at least one cue"};
    }
    std::sort(cues.begin(), cues.end(),
              [](const CaptionCue& first, const CaptionCue& second) {
                  if (first.startOffset() != second.startOffset()) {
                      return first.startOffset() < second.startOffset();
                  }
                  return first.id() < second.id();
              });
    std::set<CueId> identities;
    for (std::size_t index = 0; index < cues.size(); ++index) {
        if (!identities.insert(cues[index].id()).second) {
            return AppError{ErrorCode::AlreadyExists,
                            "caption cue id already exists"};
        }
        if (cues[index].endOffset() > clipDuration) {
            return AppError{ErrorCode::InvalidArgument,
                            "caption cue exceeds its clip"};
        }
        if (index > 0 &&
            cues[index - 1].endOffset() > cues[index].startOffset()) {
            return AppError{ErrorCode::InvalidArgument,
                            "caption cues must not overlap"};
        }
    }
    return cues;
}

}  // namespace

core::Result<Clip> Clip::createAsset(
    ClipId id, const MediaAsset& asset, TimeRange sourceRange,
    TimeRange timelineRange, bool enabled,
    std::optional<VisualTransform> visualTransform,
    std::optional<AudioEnvelope> audioEnvelope) {
    const auto sourceEnd = sourceRange.end().time_since_epoch();
    if (sourceEnd > asset.duration() ||
        sourceRange.duration() != timelineRange.duration()) {
        return AppError{ErrorCode::InvalidArgument,
                        "clip range exceeds its asset or changes speed"};
    }
    if (asset.kind() == MediaKind::Audio && visualTransform.has_value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "audio clips cannot have a visual transform"};
    }
    if (!asset.audio().has_value() && audioEnvelope.has_value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "clip has an audio envelope without an audio stream"};
    }
    return Clip{std::move(id), ClipKind::Asset, asset.id(), asset.kind(),
                asset.audio().has_value(), asset.duration(), sourceRange,
                timelineRange, enabled, std::move(visualTransform),
                std::move(audioEnvelope), std::nullopt, {}};
}

core::Result<Clip> Clip::createTitle(
    ClipId id, TimeRange timelineRange, bool enabled, TitlePayload payload,
    std::optional<VisualTransform> visualTransform) {
    auto source = TimeRange::create(core::TimestampNs{}, timelineRange.duration());
    if (!source.hasValue()) return source.error();
    return Clip{std::move(id), ClipKind::Title, std::nullopt, MediaKind::Image,
                false, timelineRange.duration(), source.value(), timelineRange,
                enabled, std::move(visualTransform), std::nullopt,
                std::move(payload), {}};
}

core::Result<Clip> Clip::createCaption(
    ClipId id, TimeRange timelineRange, bool enabled,
    std::vector<CaptionCue> cues,
    std::optional<VisualTransform> visualTransform) {
    auto validated = validatedCaptionCues(std::move(cues),
                                          timelineRange.duration());
    if (!validated.hasValue()) return validated.error();
    auto source = TimeRange::create(core::TimestampNs{}, timelineRange.duration());
    if (!source.hasValue()) return source.error();
    return Clip{std::move(id), ClipKind::Caption, std::nullopt,
                MediaKind::Image, false, timelineRange.duration(),
                source.value(), timelineRange, enabled,
                std::move(visualTransform), std::nullopt, std::nullopt,
                std::move(validated).value()};
}

core::Result<Clip> Clip::withIdentityAndRanges(
    ClipId id, TimeRange sourceRange, TimeRange timelineRange) const {
    if (kind_ != ClipKind::Asset || !assetId_.has_value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "generated clips cannot be resegmented as media"};
    }
    if (sourceRange.end().time_since_epoch() > assetDuration_ ||
        sourceRange.duration() != timelineRange.duration()) {
        return AppError{ErrorCode::InvalidArgument,
                        "resegmented clip exceeds its asset or changes speed"};
    }
    if (audioEnvelope_.has_value()) {
        auto validated = AudioEnvelope::create(
            audioEnvelope_->gainDb(), audioEnvelope_->fadeIn(),
            audioEnvelope_->fadeOut(), timelineRange.duration());
        if (!validated.hasValue()) return validated.error();
    }
    return Clip{std::move(id), kind_, assetId_, mediaKind_, hasAudio_,
                assetDuration_, sourceRange, timelineRange, enabled_,
                visualTransform_, audioEnvelope_, titlePayload_, captionCues_};
}

core::Result<Clip> Clip::withVisualTransform(
    std::optional<VisualTransform> visualTransform) const {
    if (mediaKind_ == MediaKind::Audio) {
        return AppError{ErrorCode::InvalidArgument,
                        "audio clips cannot have a visual transform"};
    }
    return Clip{id_, kind_, assetId_, mediaKind_, hasAudio_, assetDuration_,
                sourceRange_, timelineRange_, enabled_,
                std::move(visualTransform), audioEnvelope_, titlePayload_,
                captionCues_};
}

core::Result<Clip> Clip::withAudioEnvelope(
    std::optional<AudioEnvelope> audioEnvelope) const {
    if (audioEnvelope.has_value() && !hasAudio_) {
        return AppError{ErrorCode::InvalidArgument,
                        "clip has no audio stream"};
    }
    if (audioEnvelope.has_value()) {
        auto valid = AudioEnvelope::create(
            audioEnvelope->gainDb(), audioEnvelope->fadeIn(),
            audioEnvelope->fadeOut(), timelineRange_.duration());
        if (!valid.hasValue()) return valid.error();
    }
    return Clip{id_, kind_, assetId_, mediaKind_, hasAudio_, assetDuration_,
                sourceRange_, timelineRange_, enabled_, visualTransform_,
                std::move(audioEnvelope), titlePayload_, captionCues_};
}

core::Result<Clip> Clip::withTitlePayload(TitlePayload payload) const {
    if (kind_ != ClipKind::Title) {
        return AppError{ErrorCode::InvalidArgument,
                        "only title clips have title payloads"};
    }
    return Clip{id_, kind_, assetId_, mediaKind_, hasAudio_, assetDuration_,
                sourceRange_, timelineRange_, enabled_, visualTransform_,
                audioEnvelope_, std::move(payload), captionCues_};
}

core::Result<Clip> Clip::withCaptionCues(
    std::vector<CaptionCue> cues) const {
    if (kind_ != ClipKind::Caption) {
        return AppError{ErrorCode::InvalidArgument,
                        "only caption clips have caption cues"};
    }
    auto validated = validatedCaptionCues(std::move(cues),
                                          timelineRange_.duration());
    if (!validated.hasValue()) return validated.error();
    return Clip{id_, kind_, assetId_, mediaKind_, hasAudio_, assetDuration_,
                sourceRange_, timelineRange_, enabled_, visualTransform_,
                audioEnvelope_, titlePayload_, std::move(validated).value()};
}

core::Result<Track> Track::create(
    TrackId id, TrackKind kind, std::string name, bool enabled, bool locked) {
    if (name.empty()) {
        return AppError{ErrorCode::InvalidArgument, "track name must not be empty"};
    }
    return Track{std::move(id), kind, std::move(name), enabled, locked};
}

core::Result<Timeline> Timeline::create(
    TimelineId id, std::string name, core::FrameRate frameRate) {
    if (name.empty()) {
        return AppError{ErrorCode::InvalidArgument, "timeline name must not be empty"};
    }
    return Timeline{std::move(id), std::move(name), frameRate};
}

const Track* Timeline::track(const TrackId& id) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&id](const Track& candidate) {
                                        return candidate.id() == id;
                                    });
    return found == tracks_.end() ? nullptr : &*found;
}

const Clip* Timeline::clip(
    const TrackId& trackId, const ClipId& clipId) const noexcept {
    const auto* target = track(trackId);
    if (target == nullptr) return nullptr;
    const auto found = std::find_if(target->clips().begin(), target->clips().end(),
                                    [&clipId](const Clip& candidate) {
                                        return candidate.id() == clipId;
                                    });
    return found == target->clips().end() ? nullptr : &*found;
}

const TimelineMarker* Timeline::marker(const MarkerId& id) const noexcept {
    const auto found = std::ranges::find(markers_, id, &TimelineMarker::id);
    return found == markers_.end() ? nullptr : &*found;
}

Track* Timeline::mutableTrack(const TrackId& id) noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&id](const Track& candidate) {
                                        return candidate.id() == id;
                                    });
    return found == tracks_.end() ? nullptr : &*found;
}

bool Timeline::containsClipId(const ClipId& id) const noexcept {
    for (const auto& candidateTrack : tracks_) {
        const auto found = std::find_if(candidateTrack.clips().begin(),
                                        candidateTrack.clips().end(),
                                        [&id](const Clip& clip) { return clip.id() == id; });
        if (found != candidateTrack.clips().end()) return true;
    }
    return false;
}

bool Timeline::containsCueId(const CueId& id) const noexcept {
    for (const auto& candidateTrack : tracks_) {
        for (const auto& candidateClip : candidateTrack.clips()) {
            const auto found = std::find_if(
                candidateClip.captionCues().begin(),
                candidateClip.captionCues().end(),
                [&id](const CaptionCue& cue) { return cue.id() == id; });
            if (found != candidateClip.captionCues().end()) return true;
        }
    }
    return false;
}

bool Timeline::containsCueIdOutsideTrack(
    const TrackId& trackId, const CueId& id) const noexcept {
    for (const auto& candidateTrack : tracks_) {
        if (candidateTrack.id() == trackId) continue;
        for (const auto& candidateClip : candidateTrack.clips()) {
            const auto found = std::find_if(
                candidateClip.captionCues().begin(),
                candidateClip.captionCues().end(),
                [&id](const CaptionCue& cue) { return cue.id() == id; });
            if (found != candidateClip.captionCues().end()) return true;
        }
    }
    return false;
}

core::Result<void> Timeline::validateClips(
    TrackKind kind, const std::vector<Clip>& clips) {
    std::set<ClipId> identities;
    std::set<CueId> cueIdentities;
    for (std::size_t index = 0; index < clips.size(); ++index) {
        if (!compatible(kind, clips[index])) {
            return AppError{ErrorCode::InvalidArgument,
                            "clip content is incompatible with its track"};
        }
        if (!identities.insert(clips[index].id()).second) {
            return AppError{ErrorCode::AlreadyExists, "clip id already exists"};
        }
        for (const auto& cue : clips[index].captionCues()) {
            if (!cueIdentities.insert(cue.id()).second) {
                return AppError{ErrorCode::AlreadyExists,
                                "caption cue id already exists"};
            }
        }
        if (index > 0) {
            if (overlaps(clips[index - 1].timelineRange(),
                         clips[index].timelineRange())) {
                return AppError{ErrorCode::InvalidArgument,
                                "clips on one track must not overlap"};
            }
        }
    }
    return core::ok();
}

core::Result<void> Timeline::setTrackLocked(
    const TrackId& trackId, bool locked) {
    auto* target = mutableTrack(trackId);
    if (target == nullptr) {
        return AppError{ErrorCode::NotFound, "timeline track was not found"};
    }
    target->locked_ = locked;
    return core::ok();
}

core::Result<void> Timeline::addTrack(Track added) {
    if (track(added.id()) != nullptr) {
        return AppError{ErrorCode::AlreadyExists, "track id already exists"};
    }
    tracks_.push_back(std::move(added));
    return core::ok();
}

core::Result<Track> Timeline::removeTrack(const TrackId& trackId) {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&trackId](const Track& candidate) {
                                        return candidate.id() == trackId;
                                    });
    if (found == tracks_.end()) {
        return AppError{ErrorCode::NotFound, "timeline track was not found"};
    }
    if (found->locked()) {
        return AppError{ErrorCode::InvalidState, "timeline track is locked"};
    }
    if (!found->clips().empty()) {
        return AppError{ErrorCode::InvalidState,
                        "timeline track must be empty before removal"};
    }
    if (found->kind() != TrackKind::Title &&
        found->kind() != TrackKind::Caption) {
        return AppError{ErrorCode::InvalidArgument,
                        "only generated title or caption tracks may be removed"};
    }
    Track removed = std::move(*found);
    tracks_.erase(found);
    return removed;
}

core::Result<void> Timeline::addMarker(TimelineMarker added) {
    if (marker(added.id()) != nullptr) {
        return AppError{ErrorCode::AlreadyExists, "marker id already exists"};
    }
    if (std::ranges::find(markers_, added.position(),
                          &TimelineMarker::position) != markers_.end()) {
        return AppError{ErrorCode::AlreadyExists,
                        "marker position already exists"};
    }
    auto staged = markers_;
    staged.push_back(std::move(added));
    std::ranges::sort(staged, [](const TimelineMarker& first,
                                const TimelineMarker& second) {
        if (first.position() != second.position()) {
            return first.position() < second.position();
        }
        return first.id() < second.id();
    });
    markers_ = std::move(staged);
    return core::ok();
}

core::Result<TimelineMarker> Timeline::removeMarker(
    const MarkerId& markerId) {
    const auto found = std::ranges::find(markers_, markerId,
                                         &TimelineMarker::id);
    if (found == markers_.end()) {
        return AppError{ErrorCode::NotFound, "timeline marker was not found"};
    }
    TimelineMarker removed = std::move(*found);
    markers_.erase(found);
    return removed;
}

core::Result<void> Timeline::insertClip(const TrackId& trackId, Clip clip) {
    auto* target = mutableTrack(trackId);
    if (target == nullptr) {
        return AppError{ErrorCode::NotFound, "timeline track was not found"};
    }
    if (target->locked()) {
        return AppError{ErrorCode::InvalidState, "timeline track is locked"};
    }
    if (containsClipId(clip.id())) {
        return AppError{ErrorCode::AlreadyExists, "clip id already exists"};
    }
    for (const auto& cue : clip.captionCues()) {
        if (containsCueId(cue.id())) {
            return AppError{ErrorCode::AlreadyExists,
                            "caption cue id already exists"};
        }
    }

    auto staged = target->clips_;
    staged.push_back(std::move(clip));
    std::sort(staged.begin(), staged.end(), [](const Clip& first, const Clip& second) {
        if (first.timelineRange().start() != second.timelineRange().start()) {
            return first.timelineRange().start() < second.timelineRange().start();
        }
        return first.id() < second.id();
    });
    if (auto valid = validateClips(target->kind(), staged); !valid.hasValue()) {
        return valid.error();
    }
    target->clips_ = std::move(staged);
    return core::ok();
}

core::Result<void> Timeline::replaceClip(
    const TrackId& trackId, const ClipId& clipId, Clip replacement) {
    auto* target = mutableTrack(trackId);
    if (target == nullptr) {
        return AppError{ErrorCode::NotFound, "timeline track was not found"};
    }
    if (target->locked()) {
        return AppError{ErrorCode::InvalidState, "timeline track is locked"};
    }
    if (replacement.id() != clipId) {
        return AppError{ErrorCode::InvalidArgument,
                        "replacement must preserve the clip identity"};
    }
    for (const auto& cue : replacement.captionCues()) {
        if (containsCueIdOutsideTrack(trackId, cue.id())) {
            return AppError{ErrorCode::AlreadyExists,
                            "caption cue id already exists"};
        }
    }
    auto staged = target->clips_;
    const auto found = std::find_if(staged.begin(), staged.end(),
                                    [&clipId](const Clip& candidate) {
                                        return candidate.id() == clipId;
                                    });
    if (found == staged.end()) {
        return AppError{ErrorCode::NotFound, "timeline clip was not found"};
    }
    *found = std::move(replacement);
    std::sort(staged.begin(), staged.end(), [](const Clip& first, const Clip& second) {
        if (first.timelineRange().start() != second.timelineRange().start()) {
            return first.timelineRange().start() < second.timelineRange().start();
        }
        return first.id() < second.id();
    });
    if (auto valid = validateClips(target->kind(), staged); !valid.hasValue()) {
        return valid.error();
    }
    target->clips_ = std::move(staged);
    return core::ok();
}

core::Result<Clip> Timeline::removeClip(
    const TrackId& trackId, const ClipId& clipId) {
    auto* target = mutableTrack(trackId);
    if (target == nullptr) {
        return AppError{ErrorCode::NotFound, "timeline track was not found"};
    }
    if (target->locked()) {
        return AppError{ErrorCode::InvalidState, "timeline track is locked"};
    }
    const auto found = std::find_if(target->clips_.begin(), target->clips_.end(),
                                    [&clipId](const Clip& candidate) {
                                        return candidate.id() == clipId;
                                    });
    if (found == target->clips_.end()) {
        return AppError{ErrorCode::NotFound, "timeline clip was not found"};
    }
    Clip removed = std::move(*found);
    target->clips_.erase(found);
    return removed;
}

core::Result<void> Timeline::replaceTrackClips(
    const TrackId& trackId, std::vector<Clip> clips) {
    auto* target = mutableTrack(trackId);
    if (target == nullptr) {
        return AppError{ErrorCode::NotFound, "timeline track was not found"};
    }
    if (target->locked()) {
        return AppError{ErrorCode::InvalidState, "timeline track is locked"};
    }
    std::sort(clips.begin(), clips.end(), [](const Clip& first, const Clip& second) {
        if (first.timelineRange().start() != second.timelineRange().start()) {
            return first.timelineRange().start() < second.timelineRange().start();
        }
        return first.id() < second.id();
    });
    if (auto valid = validateClips(target->kind(), clips); !valid.hasValue()) {
        return valid.error();
    }
    for (const auto& clip : clips) {
        for (const auto& cue : clip.captionCues()) {
            if (containsCueIdOutsideTrack(trackId, cue.id())) {
                return AppError{ErrorCode::AlreadyExists,
                                "caption cue id already exists"};
            }
        }
    }
    for (const auto& candidateTrack : tracks_) {
        if (candidateTrack.id() == trackId) continue;
        for (const auto& candidate : clips) {
            const auto duplicate = std::find_if(
                candidateTrack.clips().begin(), candidateTrack.clips().end(),
                [&candidate](const Clip& existing) {
                    return existing.id() == candidate.id();
                });
            if (duplicate != candidateTrack.clips().end()) {
                return AppError{ErrorCode::AlreadyExists, "clip id already exists"};
            }
        }
    }
    target->clips_ = std::move(clips);
    return core::ok();
}

}  // namespace creator::domain
