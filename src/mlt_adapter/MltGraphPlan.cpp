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

}  // namespace

core::Result<MltGraphPlan> compileMltGraphPlan(
    const edit_engine::TimelineSnapshot& snapshot) {
    std::unordered_map<std::string, const domain::MediaAsset*> assets;
    assets.reserve(snapshot.assets.size());
    for (const auto& asset : snapshot.assets) {
        if (!assets.emplace(asset.id().value(), &asset).second) {
            return invalid("timeline snapshot contains duplicate media asset ids");
        }
    }

    std::vector<MltGraphTrack> tracks;
    tracks.reserve(snapshot.timeline.tracks().size());
    std::int64_t durationFrames = 0;
    const auto frameRate = snapshot.timeline.frameRate();

    for (const auto& track : snapshot.timeline.tracks()) {
        std::vector<MltGraphClip> clips;
        clips.reserve(track.clips().size());
        for (const auto& clip : track.clips()) {
            if (clip.kind() != domain::ClipKind::Asset ||
                !clip.assetId().has_value()) {
                return core::AppError{core::ErrorCode::UnsupportedVersion,
                                      "MLT graph does not support this clip kind"};
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
            const auto timelineIn =
                core::timestampToFrame(clip.timelineRange().start(), frameRate);
            const auto timelineEnd =
                core::timestampToFrame(clip.timelineRange().end(), frameRate);
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

            const auto timelineOut = timelineEnd - 1;
            durationFrames = std::max(durationFrames, timelineOut + 1);
            clips.push_back(MltGraphClip{
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
                clip.audioEnvelope()});
        }
        tracks.push_back(MltGraphTrack{track.id(), track.kind(), track.enabled(),
                                       std::move(clips)});
    }

    return MltGraphPlan{frameRate, snapshot.revision, std::move(tracks),
                        durationFrames};
}

}  // namespace creator::mlt_adapter
