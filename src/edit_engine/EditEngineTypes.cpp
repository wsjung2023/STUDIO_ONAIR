#include "edit_engine/EditEngineTypes.h"

#include "core/Uuid.h"

#include "core/AppError.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <system_error>
#include <string_view>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

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

bool isPresetId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char value) {
        return std::islower(value) != 0 || std::isdigit(value) != 0 ||
               value == '-';
    });
}

bool isMp4Extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension == ".mp4";
}

bool isDevicePath(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto& native = path.native();
    return native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\");
#else
    static_cast<void>(path);
    return false;
#endif
}

bool isReparsePoint(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, error));
#endif
}

bool isTerminal(RenderJobState state) noexcept {
    return state == RenderJobState::Completed ||
           state == RenderJobState::Failed ||
           state == RenderJobState::Cancelled;
}

bool isAllowedStateTransition(RenderJobState previous,
                              RenderJobState next) noexcept {
    if (previous == next) return !isTerminal(previous);
    switch (previous) {
        case RenderJobState::Pending:
            return next == RenderJobState::Running ||
                   next == RenderJobState::Cancelling ||
                   next == RenderJobState::Failed;
        case RenderJobState::Running:
            return next == RenderJobState::Publishing ||
                   next == RenderJobState::Cancelling ||
                   next == RenderJobState::Failed;
        case RenderJobState::Publishing:
            return next == RenderJobState::Completed ||
                   next == RenderJobState::Cancelled ||
                   next == RenderJobState::Failed;
        case RenderJobState::Cancelling:
            return next == RenderJobState::Cancelled ||
                   next == RenderJobState::Completed ||
                   next == RenderJobState::Failed;
        case RenderJobState::Completed:
        case RenderJobState::Failed:
        case RenderJobState::Cancelled:
            return false;
    }
    return false;
}

bool isGeneratedCachePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory() || path.filename().empty() ||
        containsParentTraversal(path) || path.lexically_normal() != path) {
        return false;
    }
    auto component = path.begin();
    if (component == path.end() || *component != "cache") return false;
    ++component;
    if (component == path.end() || *component != "generated") return false;
    ++component;
    return component != path.end();
}

bool contains(const domain::TimeRange& outer,
              const domain::TimeRange& inner) noexcept {
    return inner.start() >= outer.start() && inner.end() <= outer.end();
}

}  // namespace

Result<GeneratedOverlayDescriptor> GeneratedOverlayDescriptor::create(
    domain::ClipId ownerClipId, std::optional<domain::CueId> cueId,
    std::filesystem::path rasterPath, domain::TimeRange timelineRange,
    std::string resolvedFontFamily) {
    if (!isGeneratedCachePath(rasterPath)) {
        return invalid(
            "generated overlay path must be normalized below cache/generated");
    }
    if (resolvedFontFamily.empty() || resolvedFontFamily.size() > 128) {
        return invalid("generated overlay font family must be bounded");
    }
    return GeneratedOverlayDescriptor{
        std::move(ownerClipId), std::move(cueId), std::move(rasterPath),
        timelineRange, std::move(resolvedFontFamily)};
}

Result<void> validateTimelineSnapshot(const TimelineSnapshot& snapshot) {
    constexpr std::int32_t kMinimumCanvasDimension = 16;
    constexpr std::int32_t kMaximumCanvasDimension = 16'384;
    if (snapshot.canvasWidth < kMinimumCanvasDimension ||
        snapshot.canvasWidth > kMaximumCanvasDimension ||
        snapshot.canvasHeight < kMinimumCanvasDimension ||
        snapshot.canvasHeight > kMaximumCanvasDimension) {
        return invalid("timeline snapshot canvas dimensions are invalid");
    }
    for (const auto& descriptor : snapshot.generatedOverlays) {
        const domain::Clip* owner = nullptr;
        for (const auto& track : snapshot.timeline.tracks()) {
            for (const auto& clip : track.clips()) {
                if (clip.id() == descriptor.ownerClipId()) {
                    owner = &clip;
                    break;
                }
            }
            if (owner != nullptr) break;
        }
        if (owner == nullptr || owner->kind() == domain::ClipKind::Asset ||
            !contains(owner->timelineRange(), descriptor.timelineRange())) {
            return invalid(
                "generated overlay must stay inside its generated owner clip");
        }
        if (owner->kind() == domain::ClipKind::Title) {
            if (descriptor.cueId().has_value()) {
                return invalid("title overlay must not identify a caption cue");
            }
            continue;
        }
        if (!descriptor.cueId().has_value()) {
            return invalid("caption overlay must identify its caption cue");
        }
        const auto cue = std::find_if(
            owner->captionCues().begin(), owner->captionCues().end(),
            [&](const domain::CaptionCue& value) {
                return value.id() == *descriptor.cueId();
            });
        if (cue == owner->captionCues().end()) {
            return invalid("caption overlay identifies an unknown cue");
        }
        const auto cueRange = domain::TimeRange::create(
            owner->timelineRange().start() + cue->startOffset(), cue->duration());
        if (!cueRange.hasValue() ||
            !contains(cueRange.value(), descriptor.timelineRange())) {
            return invalid("caption overlay must stay inside its caption cue");
        }
    }
    return core::ok();
}

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
    if (auto validated = validateTimelineSnapshot(target);
        !validated.hasValue()) {
        return validated.error();
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
    std::string id, std::uint32_t width, std::uint32_t height,
    core::FrameRate frameRate, std::uint32_t videoBitrate,
    std::uint32_t audioBitrate, RenderFallbackPolicy fallbackPolicy) {
    constexpr std::uint32_t kMaximumDimension = 16'384;
    if (!isPresetId(id) || width == 0 || height == 0 ||
        width > kMaximumDimension ||
        height > kMaximumDimension || videoBitrate == 0 || audioBitrate == 0) {
        return invalid("render preset id, dimensions, and bitrates must be valid");
    }
    return RenderPreset{std::move(id), width, height, frameRate, videoBitrate,
                        audioBitrate, fallbackPolicy};
}

Result<RenderPreset> RenderPreset::h2641080p30() {
    auto frameRate = core::FrameRate::create(30, 1);
    if (!frameRate.hasValue()) return frameRate.error();
    return create("h264-1080p30", 1920, 1080, frameRate.value(), 12'000'000,
                  192'000, RenderFallbackPolicy::HardwareThenSoftware);
}

Result<RenderPreset> RenderPreset::h2642160p30() {
    auto frameRate = core::FrameRate::create(30, 1);
    if (!frameRate.hasValue()) return frameRate.error();
    return create("h264-2160p30", 3840, 2160, frameRate.value(), 45'000'000,
                  256'000, RenderFallbackPolicy::HardwareThenSoftware);
}

Result<RenderRequest> RenderRequest::create(
    domain::ProjectId projectId, TimelineSnapshot snapshot,
    std::filesystem::path destination, RenderPreset preset,
    RenderOverwritePolicy overwritePolicy) {
    if (destination.empty() || !destination.is_absolute() ||
        destination.filename().empty() || containsParentTraversal(destination) ||
        !isMp4Extension(destination) || isDevicePath(destination)) {
        return invalid("render destination must be an absolute safe MP4 path");
    }
    std::error_code error;
    const auto parent = destination.parent_path();
    if (!std::filesystem::is_directory(parent, error) || error ||
        isReparsePoint(parent)) {
        return invalid("render destination parent must be an existing regular directory");
    }
    const bool destinationExists = std::filesystem::exists(destination, error);
    if (error) return invalid("render destination could not be inspected");
    if (destinationExists) {
        if (overwritePolicy == RenderOverwritePolicy::FailIfExists ||
            !std::filesystem::is_regular_file(destination, error) || error ||
            isReparsePoint(destination)) {
            return invalid("render destination already exists or is unsafe");
        }
    }
    if (auto validated = validateTimelineSnapshot(snapshot);
        !validated.hasValue()) {
        return validated.error();
    }
    const bool hasClip = std::any_of(
        snapshot.timeline.tracks().begin(), snapshot.timeline.tracks().end(),
        [](const domain::Track& track) { return !track.clips().empty(); });
    if (!hasClip) {
        return invalid("render timeline must not be empty");
    }
    auto jobId = domain::RenderJobId::create(core::generateUuidV4());
    if (!jobId.hasValue()) return jobId.error();
    return RenderRequest{std::move(jobId).value(), std::move(projectId),
                         std::move(snapshot),
                         std::move(destination), std::move(preset),
                         overwritePolicy};
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
        ((state == RenderJobState::Running ||
          state == RenderJobState::Cancelling ||
          state == RenderJobState::Failed ||
          state == RenderJobState::Cancelled) &&
         fraction >= 1.0) ||
        (state == RenderJobState::Publishing &&
         (fraction >= 1.0 || rendered != total)) ||
        (state == RenderJobState::Completed &&
         (fraction != 1.0 || rendered != total))) {
        return invalid("render progress contradicts its state");
    }
    return RenderProgress{state, fraction, renderedThrough, totalDuration};
}

Result<void> validateRenderProgressTransition(const RenderProgress& previous,
                                              const RenderProgress& next) {
    if (isTerminal(previous.state())) {
        if (previous == next) return core::ok();
        return invalid("terminal render progress is immutable");
    }
    if (!isAllowedStateTransition(previous.state(), next.state())) {
        return invalid("render progress state transition is not allowed");
    }
    if (previous.totalDuration() != next.totalDuration() ||
        next.fraction() < previous.fraction() ||
        next.renderedThrough() < previous.renderedThrough()) {
        return invalid("render progress must not regress");
    }
    return core::ok();
}

}  // namespace creator::edit_engine
