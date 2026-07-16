#include "edit_engine/EditEngineTypes.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace creator::edit_engine {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError invalid(std::string_view message) {
    return AppError{ErrorCode::InvalidArgument, std::string{message}};
}

bool containsParentTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

}  // namespace

Result<TimelineChangeSet> TimelineChangeSet::create(
    domain::TimelineRevision baseRevision, TimelineSnapshot target,
    std::vector<domain::TrackId> affectedTracks, bool requiresFullRebuild) {
    auto next = baseRevision.next();
    if (!next.hasValue() || target.revision != next.value()) {
        return invalid("timeline change target must be the next revision");
    }
    if (affectedTracks.size() > kMaxAffectedTracks) {
        return invalid("timeline change exceeds the affected track limit");
    }
    if (!requiresFullRebuild && affectedTracks.empty()) {
        return invalid("incremental timeline change needs an affected track");
    }
    std::vector<std::string_view> identities;
    identities.reserve(affectedTracks.size());
    for (const auto& id : affectedTracks) identities.push_back(id.value());
    std::sort(identities.begin(), identities.end());
    if (std::adjacent_find(identities.begin(), identities.end()) !=
        identities.end()) {
        return invalid("timeline change contains a duplicate track identity");
    }
    return TimelineChangeSet{baseRevision, std::move(target),
                             std::move(affectedTracks), requiresFullRebuild};
}

Result<PreviewFrame> PreviewFrame::create(
    core::TimestampNs position, domain::TimelineRevision revision,
    media::VideoFrame frame) {
    const auto positionNs = position.time_since_epoch().count();
    if (positionNs < 0 || frame.timestamp != position || frame.width == 0 ||
        frame.height == 0 || frame.contentWidth == 0 ||
        frame.contentHeight == 0 || frame.visibleRect.width == 0 ||
        frame.visibleRect.height == 0 || frame.visibleRect.x > frame.width ||
        frame.visibleRect.width > frame.width - frame.visibleRect.x ||
        frame.visibleRect.y > frame.height ||
        frame.visibleRect.height > frame.height - frame.visibleRect.y ||
        !std::isfinite(frame.contentScale) || frame.contentScale <= 0.0 ||
        !std::isfinite(frame.pointPixelScale) || frame.pointPixelScale <= 0.0 ||
        frame.pixelFormat == media::PixelFormat::Unknown) {
        return invalid("preview frame geometry or timestamp is invalid");
    }
    return PreviewFrame{position, revision, std::move(frame)};
}

Result<RenderPreset> RenderPreset::create(
    std::uint32_t width, std::uint32_t height, core::FrameRate frameRate,
    std::uint32_t videoBitrate, std::uint32_t audioBitrate) {
    constexpr std::uint32_t kMaximumDimension = 16'384;
    if (width == 0 || height == 0 || width > kMaximumDimension ||
        height > kMaximumDimension || videoBitrate == 0 || audioBitrate == 0) {
        return invalid("render preset dimensions and bitrates must be bounded");
    }
    return RenderPreset{width, height, frameRate, videoBitrate, audioBitrate};
}

Result<RenderRequest> RenderRequest::create(
    TimelineSnapshot snapshot, std::filesystem::path destination,
    RenderPreset preset) {
    if (destination.empty() || destination.filename().empty() ||
        containsParentTraversal(destination)) {
        return invalid("render destination is empty or contains traversal");
    }
    return RenderRequest{std::move(snapshot), std::move(destination), preset};
}

Result<RenderProgress> RenderProgress::create(
    RenderJobState state, double fraction, core::TimestampNs renderedThrough,
    core::DurationNs totalDuration) {
    const auto rendered = renderedThrough.time_since_epoch().count();
    const auto total = totalDuration.count();
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 ||
        rendered < 0 || total <= 0 || rendered > total) {
        return invalid("render progress is outside its valid range");
    }
    if ((state == RenderJobState::Pending &&
         (fraction != 0.0 || rendered != 0)) ||
        (state == RenderJobState::Running && fraction >= 1.0) ||
        (state == RenderJobState::Completed &&
         (fraction != 1.0 || rendered != total))) {
        return invalid("render progress contradicts its state");
    }
    return RenderProgress{state, fraction, renderedThrough, totalDuration};
}

}  // namespace creator::edit_engine
