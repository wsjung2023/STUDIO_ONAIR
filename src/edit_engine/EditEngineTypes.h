#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "media/MediaTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace creator::edit_engine {

class GeneratedOverlayDescriptor final {
public:
    [[nodiscard]] static core::Result<GeneratedOverlayDescriptor> create(
        domain::ClipId ownerClipId, std::optional<domain::CueId> cueId,
        std::filesystem::path rasterPath, domain::TimeRange timelineRange,
        std::string resolvedFontFamily);

    [[nodiscard]] const domain::ClipId& ownerClipId() const noexcept {
        return ownerClipId_;
    }
    [[nodiscard]] const std::optional<domain::CueId>& cueId() const noexcept {
        return cueId_;
    }
    [[nodiscard]] const std::filesystem::path& rasterPath() const noexcept {
        return rasterPath_;
    }
    [[nodiscard]] const domain::TimeRange& timelineRange() const noexcept {
        return timelineRange_;
    }
    [[nodiscard]] const std::string& resolvedFontFamily() const noexcept {
        return resolvedFontFamily_;
    }

    friend bool operator==(const GeneratedOverlayDescriptor&,
                           const GeneratedOverlayDescriptor&) = default;

private:
    GeneratedOverlayDescriptor(domain::ClipId ownerClipId,
                               std::optional<domain::CueId> cueId,
                               std::filesystem::path rasterPath,
                               domain::TimeRange timelineRange,
                               std::string resolvedFontFamily)
        : ownerClipId_(std::move(ownerClipId)), cueId_(std::move(cueId)),
          rasterPath_(std::move(rasterPath)), timelineRange_(timelineRange),
          resolvedFontFamily_(std::move(resolvedFontFamily)) {}

    domain::ClipId ownerClipId_;
    std::optional<domain::CueId> cueId_;
    std::filesystem::path rasterPath_;
    domain::TimeRange timelineRange_;
    std::string resolvedFontFamily_;
};

struct TimelineSnapshot final {
    domain::Timeline timeline;
    domain::TimelineRevision revision;
    std::vector<domain::MediaAsset> assets{};
    std::filesystem::path mediaRoot{};
    std::int32_t canvasWidth{1920};
    std::int32_t canvasHeight{1080};
    std::vector<GeneratedOverlayDescriptor> generatedOverlays{};

    friend bool operator==(const TimelineSnapshot&,
                           const TimelineSnapshot&) = default;
};

[[nodiscard]] core::Result<void> validateTimelineSnapshot(
    const TimelineSnapshot& snapshot);

class TimelineChangeSet final {
public:
    static constexpr std::size_t kMaxAffectedTracks = 256;

    [[nodiscard]] static core::Result<TimelineChangeSet> create(
        domain::TimelineRevision baseRevision, TimelineSnapshot target,
        std::vector<domain::TrackId> affectedTracks,
        bool requiresFullRebuild);

    [[nodiscard]] domain::TimelineRevision baseRevision() const noexcept {
        return baseRevision_;
    }
    [[nodiscard]] const TimelineSnapshot& target() const noexcept {
        return target_;
    }
    [[nodiscard]] const std::vector<domain::TrackId>& affectedTracks() const noexcept {
        return affectedTracks_;
    }
    [[nodiscard]] bool requiresFullRebuild() const noexcept {
        return requiresFullRebuild_;
    }

private:
    TimelineChangeSet(domain::TimelineRevision baseRevision,
                      TimelineSnapshot target,
                      std::vector<domain::TrackId> affectedTracks,
                      bool requiresFullRebuild)
        : baseRevision_(baseRevision),
          target_(std::move(target)),
          affectedTracks_(std::move(affectedTracks)),
          requiresFullRebuild_(requiresFullRebuild) {}

    domain::TimelineRevision baseRevision_;
    TimelineSnapshot target_;
    std::vector<domain::TrackId> affectedTracks_;
    bool requiresFullRebuild_;
};

class PreviewFrame final {
public:
    [[nodiscard]] static core::Result<PreviewFrame> create(
        core::TimestampNs position, domain::TimelineRevision revision,
        media::VideoFrame frame);

    [[nodiscard]] core::TimestampNs position() const noexcept { return position_; }
    [[nodiscard]] domain::TimelineRevision revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] const media::VideoFrame& frame() const noexcept { return frame_; }

private:
    PreviewFrame(core::TimestampNs position,
                 domain::TimelineRevision revision, media::VideoFrame frame)
        : position_(position), revision_(revision), frame_(std::move(frame)) {}

    core::TimestampNs position_;
    domain::TimelineRevision revision_;
    media::VideoFrame frame_;
};

class RenderPreset final {
public:
    [[nodiscard]] static core::Result<RenderPreset> create(
        std::uint32_t width, std::uint32_t height, core::FrameRate frameRate,
        std::uint32_t videoBitrate, std::uint32_t audioBitrate);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] core::FrameRate frameRate() const noexcept { return frameRate_; }
    [[nodiscard]] std::uint32_t videoBitrate() const noexcept {
        return videoBitrate_;
    }
    [[nodiscard]] std::uint32_t audioBitrate() const noexcept {
        return audioBitrate_;
    }

private:
    RenderPreset(std::uint32_t width, std::uint32_t height,
                 core::FrameRate frameRate, std::uint32_t videoBitrate,
                 std::uint32_t audioBitrate)
        : width_(width),
          height_(height),
          frameRate_(frameRate),
          videoBitrate_(videoBitrate),
          audioBitrate_(audioBitrate) {}

    std::uint32_t width_;
    std::uint32_t height_;
    core::FrameRate frameRate_;
    std::uint32_t videoBitrate_;
    std::uint32_t audioBitrate_;
};

class RenderRequest final {
public:
    [[nodiscard]] static core::Result<RenderRequest> create(
        TimelineSnapshot snapshot, std::filesystem::path destination,
        RenderPreset preset);

    [[nodiscard]] const TimelineSnapshot& snapshot() const noexcept {
        return snapshot_;
    }
    [[nodiscard]] const std::filesystem::path& destination() const noexcept {
        return destination_;
    }
    [[nodiscard]] const RenderPreset& preset() const noexcept { return preset_; }

private:
    RenderRequest(TimelineSnapshot snapshot, std::filesystem::path destination,
                  RenderPreset preset)
        : snapshot_(std::move(snapshot)),
          destination_(std::move(destination)),
          preset_(preset) {}

    TimelineSnapshot snapshot_;
    std::filesystem::path destination_;
    RenderPreset preset_;
};

enum class RenderJobState { Pending, Running, Completed, Failed, Cancelled };

class RenderProgress final {
public:
    [[nodiscard]] static core::Result<RenderProgress> create(
        RenderJobState state, double fraction, core::TimestampNs renderedThrough,
        core::DurationNs totalDuration);

    [[nodiscard]] RenderJobState state() const noexcept { return state_; }
    [[nodiscard]] double fraction() const noexcept { return fraction_; }
    [[nodiscard]] core::TimestampNs renderedThrough() const noexcept {
        return renderedThrough_;
    }
    [[nodiscard]] core::DurationNs totalDuration() const noexcept {
        return totalDuration_;
    }

private:
    RenderProgress(RenderJobState state, double fraction,
                   core::TimestampNs renderedThrough,
                   core::DurationNs totalDuration)
        : state_(state),
          fraction_(fraction),
          renderedThrough_(renderedThrough),
          totalDuration_(totalDuration) {}

    RenderJobState state_;
    double fraction_;
    core::TimestampNs renderedThrough_;
    core::DurationNs totalDuration_;
};

}  // namespace creator::edit_engine
