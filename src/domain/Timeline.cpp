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
    return Clip{std::move(id), asset.id(), asset.kind(), asset.duration(), sourceRange,
                timelineRange, enabled, std::move(visualTransform),
                std::move(audioEnvelope)};
}

core::Result<Clip> Clip::withIdentityAndRanges(
    ClipId id, TimeRange sourceRange, TimeRange timelineRange) const {
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
    return Clip{std::move(id), *assetId_, mediaKind_, assetDuration_, sourceRange,
                timelineRange, enabled_, visualTransform_, audioEnvelope_};
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

core::Result<void> Timeline::validateClips(
    TrackKind kind, const std::vector<Clip>& clips) {
    std::set<ClipId> identities;
    for (std::size_t index = 0; index < clips.size(); ++index) {
        if (!compatible(kind, clips[index])) {
            return AppError{ErrorCode::InvalidArgument,
                            "clip content is incompatible with its track"};
        }
        if (!identities.insert(clips[index].id()).second) {
            return AppError{ErrorCode::AlreadyExists, "clip id already exists"};
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
