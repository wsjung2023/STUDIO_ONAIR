#include "mlt_adapter/MltGraphPlan.h"

#include "core/AppError.h"

#include <algorithm>
#include <system_error>
#include <unordered_map>

namespace creator::mlt_adapter {
namespace {

core::AppError invalid(std::string message) {
    return core::AppError{core::ErrorCode::InvalidArgument, std::move(message)};
}

core::AppError ioFailure(std::string message) {
    return core::AppError{core::ErrorCode::IoFailure, std::move(message)};
}

bool escapesRoot(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) return true;
    return relative.begin() != relative.end() && *relative.begin() == "..";
}

core::Result<std::filesystem::path> resolveMediaPath(
    const std::filesystem::path& root, const domain::MediaAsset& asset) {
    if (root.empty()) return invalid("timeline snapshot media root is empty");

    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error || !std::filesystem::is_directory(canonicalRoot, error) || error) {
        return ioFailure("timeline snapshot media root is unavailable");
    }

    const std::u8string relativeUtf8{asset.relativePath().begin(),
                                     asset.relativePath().end()};
    const std::filesystem::path relativePath{relativeUtf8};
    if (relativePath.empty() || relativePath.is_absolute()) {
        return invalid("media asset path must be package-relative");
    }
    const auto candidate =
        std::filesystem::weakly_canonical(canonicalRoot / relativePath, error);
    if (error) return ioFailure("media asset path could not be resolved");
    const auto relativeToRoot = candidate.lexically_relative(canonicalRoot);
    if (escapesRoot(relativeToRoot)) {
        return invalid("media asset path escapes the package root");
    }
    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return core::AppError{core::ErrorCode::NotFound,
                              "available media asset file is missing"};
    }
    return candidate;
}

core::Result<std::pair<std::filesystem::path, bool>> resolveGeneratedPath(
    const std::filesystem::path& root,
    const edit_engine::GeneratedOverlayDescriptor& descriptor) {
    if (root.empty()) return invalid("timeline snapshot media root is empty");

    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error || !std::filesystem::is_directory(canonicalRoot, error) || error) {
        return ioFailure("timeline snapshot media root is unavailable");
    }
    const auto candidate = std::filesystem::weakly_canonical(
        canonicalRoot / descriptor.rasterPath(), error);
    if (error) return ioFailure("generated overlay path could not be resolved");
    const auto relativeToRoot = candidate.lexically_relative(canonicalRoot);
    if (escapesRoot(relativeToRoot)) {
        return invalid("generated overlay path escapes the package root");
    }
    const bool available = std::filesystem::is_regular_file(candidate, error);
    if (error) return ioFailure("generated overlay file could not be inspected");
    return std::pair{candidate, available};
}

core::Result<domain::VisualTransform> identityTransform() {
    return domain::VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 0);
}

struct FrameRange final {
    std::int64_t first;
    std::int64_t last;
};

core::Result<FrameRange> toFrames(const domain::TimeRange& range,
                                  core::FrameRate frameRate) {
    const auto first = core::timestampToFrame(range.start(), frameRate);
    const auto end = core::timestampToFrame(range.end(), frameRate);
    if (first < 0 || end <= first) {
        return invalid("timeline range does not span complete frames");
    }
    return FrameRange{first, end - 1};
}

std::string generatedIdentity(const domain::Clip& clip,
                              const std::optional<domain::CueId>& cueId) {
    if (!cueId.has_value()) return clip.id().value();
    return clip.id().value() + ":" + cueId->value();
}

}  // namespace

core::Result<MltGraphPlan> compileMltGraphPlan(
    const edit_engine::TimelineSnapshot& snapshot) {
    if (auto validated = edit_engine::validateTimelineSnapshot(snapshot);
        !validated.hasValue()) {
        return validated.error();
    }

    std::unordered_map<std::string, const domain::MediaAsset*> assets;
    assets.reserve(snapshot.assets.size());
    for (const auto& asset : snapshot.assets) {
        if (!assets.emplace(asset.id().value(), &asset).second) {
            return invalid("timeline snapshot contains duplicate media asset ids");
        }
    }

    std::vector<MltGraphTrack> tracks;
    tracks.reserve(snapshot.timeline.tracks().size());
    std::vector<MltGraphTrack> audioTracks;
    std::vector<MltVisualBranch> visualBranches;
    std::vector<std::string> diagnostics;
    std::int64_t durationFrames = 0;
    const auto frameRate = snapshot.timeline.frameRate();

    auto identity = identityTransform();
    if (!identity.hasValue()) return identity.error();

    for (std::size_t trackPosition = 0;
         trackPosition < snapshot.timeline.tracks().size(); ++trackPosition) {
        const auto& track = snapshot.timeline.tracks()[trackPosition];
        std::vector<MltGraphClip> clips;
        clips.reserve(track.clips().size());
        std::vector<MltGraphClip> audioClips;
        audioClips.reserve(track.clips().size());
        for (const auto& clip : track.clips()) {
            auto timelineFrames = toFrames(clip.timelineRange(), frameRate);
            if (!timelineFrames.hasValue()) return timelineFrames.error();
            durationFrames = std::max(durationFrames,
                                      timelineFrames.value().last + 1);

            if (clip.kind() != domain::ClipKind::Asset) {
                std::vector<std::pair<std::optional<domain::CueId>,
                                      domain::TimeRange>> generatedRanges;
                if (clip.kind() == domain::ClipKind::Title) {
                    generatedRanges.emplace_back(std::nullopt,
                                                 clip.timelineRange());
                } else {
                    generatedRanges.reserve(clip.captionCues().size());
                    for (const auto& cue : clip.captionCues()) {
                        auto range = domain::TimeRange::create(
                            clip.timelineRange().start() + cue.startOffset(),
                            cue.duration());
                        if (!range.hasValue()) return range.error();
                        generatedRanges.emplace_back(cue.id(), range.value());
                    }
                }

                for (const auto& [cueId, range] : generatedRanges) {
                    const edit_engine::GeneratedOverlayDescriptor* match = nullptr;
                    for (const auto& descriptor : snapshot.generatedOverlays) {
                        if (descriptor.ownerClipId() != clip.id() ||
                            descriptor.cueId() != cueId) {
                            continue;
                        }
                        if (descriptor.timelineRange() != range) {
                            return invalid(
                                "generated overlay range must exactly match its owner");
                        }
                        if (match != nullptr) {
                            return invalid(
                                "generated overlay owner has duplicate rasters");
                        }
                        match = &descriptor;
                    }

                    std::filesystem::path sourcePath;
                    bool available = false;
                    if (match != nullptr) {
                        auto resolved = resolveGeneratedPath(snapshot.mediaRoot,
                                                             *match);
                        if (!resolved.hasValue()) return resolved.error();
                        auto resolvedValue = std::move(resolved).value();
                        sourcePath = std::move(resolvedValue.first);
                        available = resolvedValue.second;
                    }
                    if (!available) {
                        diagnostics.push_back(
                            "generated overlay unavailable for " +
                            generatedIdentity(clip, cueId));
                    }

                    auto frames = toFrames(range, frameRate);
                    if (!frames.hasValue()) return frames.error();
                    const auto transform = identity.value();
                    const auto zOrder = clip.visualTransform().has_value()
                                            ? clip.visualTransform()->zOrder()
                                            : 0;
                    visualBranches.push_back(MltVisualBranch{
                        clip.id(), cueId, std::nullopt,
                        MltVisualSourceKind::Generated,
                        domain::MediaKind::Image, std::move(sourcePath),
                        available, track.enabled() && clip.enabled(), 0,
                        frames.value().last - frames.value().first,
                        frames.value().first, frames.value().last, transform,
                        MltVisualOrderKey{
                            zOrder, trackPosition,
                            frames.value().first,
                            generatedIdentity(clip, cueId)}});
                }
                continue;
            }
            if (!clip.assetId().has_value()) {
                return invalid("asset clip does not identify a media asset");
            }
            const auto found = assets.find(clip.assetId()->value());
            if (found == assets.end()) {
                return core::AppError{core::ErrorCode::NotFound,
                                      "timeline clip references an unknown asset"};
            }
            const auto& asset = *found->second;
            if (asset.kind() != clip.mediaKind()) {
                return invalid("timeline clip and media catalog kinds disagree");
            }

            const auto sourceIn =
                core::timestampToFrame(clip.sourceRange().start(), frameRate);
            const auto sourceEnd =
                core::timestampToFrame(clip.sourceRange().end(), frameRate);
            const auto timelineIn = timelineFrames.value().first;
            const auto timelineEnd = timelineFrames.value().last + 1;
            if (sourceIn < 0 || timelineIn < 0 || sourceEnd <= sourceIn ||
                timelineEnd <= timelineIn ||
                sourceEnd - sourceIn != timelineEnd - timelineIn) {
                return invalid("timeline clip does not span complete matching frames");
            }

            std::filesystem::path mediaPath;
            const bool available =
                asset.availability() == domain::AssetAvailability::Available;
            if (available) {
                auto resolved = resolveMediaPath(snapshot.mediaRoot, asset);
                if (!resolved.hasValue()) return resolved.error();
                mediaPath = std::move(resolved).value();
            }

            const auto timelineOut = timelineFrames.value().last;
            MltGraphClip graphClip{
                clip.id(),
                *clip.assetId(),
                clip.mediaKind(),
                std::move(mediaPath),
                available,
                clip.enabled(),
                sourceIn,
                sourceEnd - 1,
                timelineIn,
                timelineOut,
                clip.visualTransform(),
                clip.audioEnvelope()};
            clips.push_back(graphClip);
            if (clip.hasAudio()) audioClips.push_back(graphClip);

            if (track.kind() == domain::TrackKind::Video) {
                const auto transform =
                    clip.visualTransform().value_or(identity.value());
                visualBranches.push_back(MltVisualBranch{
                    clip.id(), std::nullopt, *clip.assetId(),
                    MltVisualSourceKind::Asset, clip.mediaKind(),
                    graphClip.mediaPath,
                    graphClip.available, track.enabled() && clip.enabled(),
                    graphClip.sourceIn, graphClip.sourceOut,
                    graphClip.timelineIn, graphClip.timelineOut, transform,
                    MltVisualOrderKey{transform.zOrder(), trackPosition,
                                      graphClip.timelineIn,
                                      clip.id().value()}});
            }
        }
        auto graphTrack = MltGraphTrack{track.id(), track.kind(),
                                        track.enabled(), std::move(clips)};
        if (track.enabled() && !audioClips.empty()) {
            audioTracks.push_back(MltGraphTrack{
                track.id(), track.kind(), true, std::move(audioClips)});
        }
        tracks.push_back(std::move(graphTrack));
    }

    std::sort(visualBranches.begin(), visualBranches.end(),
              [](const MltVisualBranch& left,
                 const MltVisualBranch& right) {
                  if (left.order.zOrder != right.order.zOrder) {
                      return left.order.zOrder < right.order.zOrder;
                  }
                  if (left.order.trackPosition != right.order.trackPosition) {
                      return left.order.trackPosition < right.order.trackPosition;
                  }
                  if (left.order.timelineStart != right.order.timelineStart) {
                      return left.order.timelineStart < right.order.timelineStart;
                  }
                  return left.order.identity < right.order.identity;
              });

    return MltGraphPlan{frameRate,
                        snapshot.revision,
                        snapshot.canvasWidth,
                        snapshot.canvasHeight,
                        std::move(tracks),
                        std::move(audioTracks),
                        std::move(visualBranches),
                        std::move(diagnostics),
                        durationFrames};
}

}  // namespace creator::mlt_adapter
