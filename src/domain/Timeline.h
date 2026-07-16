#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/TimelineTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace creator::domain {

enum class TrackKind { Video, Audio, Title, Caption };
enum class ClipKind { Asset, Title, Caption };

class Clip final {
public:
    [[nodiscard]] static core::Result<Clip> createAsset(
        ClipId id, const MediaAsset& asset, TimeRange sourceRange,
        TimeRange timelineRange, bool enabled,
        std::optional<VisualTransform> visualTransform,
        std::optional<AudioEnvelope> audioEnvelope);

    [[nodiscard]] const ClipId& id() const noexcept { return id_; }
    [[nodiscard]] ClipKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::optional<AssetId>& assetId() const noexcept { return assetId_; }
    [[nodiscard]] MediaKind mediaKind() const noexcept { return mediaKind_; }
    [[nodiscard]] const TimeRange& sourceRange() const noexcept { return sourceRange_; }
    [[nodiscard]] const TimeRange& timelineRange() const noexcept { return timelineRange_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] const std::optional<VisualTransform>& visualTransform() const noexcept {
        return visualTransform_;
    }
    [[nodiscard]] const std::optional<AudioEnvelope>& audioEnvelope() const noexcept {
        return audioEnvelope_;
    }

    friend bool operator==(const Clip&, const Clip&) = default;

private:
    Clip(ClipId id, AssetId assetId, MediaKind mediaKind, TimeRange sourceRange,
         TimeRange timelineRange, bool enabled,
         std::optional<VisualTransform> visualTransform,
         std::optional<AudioEnvelope> audioEnvelope)
        : id_(std::move(id)),
          kind_(ClipKind::Asset),
          assetId_(std::move(assetId)),
          mediaKind_(mediaKind),
          sourceRange_(sourceRange),
          timelineRange_(timelineRange),
          enabled_(enabled),
          visualTransform_(std::move(visualTransform)),
          audioEnvelope_(std::move(audioEnvelope)) {}

    ClipId id_;
    ClipKind kind_;
    std::optional<AssetId> assetId_;
    MediaKind mediaKind_;
    TimeRange sourceRange_;
    TimeRange timelineRange_;
    bool enabled_;
    std::optional<VisualTransform> visualTransform_;
    std::optional<AudioEnvelope> audioEnvelope_;
};

class Track final {
public:
    [[nodiscard]] static core::Result<Track> create(
        TrackId id, TrackKind kind, std::string name, bool enabled, bool locked);

    [[nodiscard]] const TrackId& id() const noexcept { return id_; }
    [[nodiscard]] TrackKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool locked() const noexcept { return locked_; }
    [[nodiscard]] const std::vector<Clip>& clips() const noexcept { return clips_; }

    friend bool operator==(const Track&, const Track&) = default;

private:
    friend class Timeline;

    Track(TrackId id, TrackKind kind, std::string name, bool enabled, bool locked)
        : id_(std::move(id)),
          kind_(kind),
          name_(std::move(name)),
          enabled_(enabled),
          locked_(locked) {}

    TrackId id_;
    TrackKind kind_;
    std::string name_;
    bool enabled_;
    bool locked_;
    std::vector<Clip> clips_;
};

class Timeline final {
public:
    [[nodiscard]] static core::Result<Timeline> create(
        TimelineId id, std::string name, core::FrameRate frameRate);

    [[nodiscard]] const TimelineId& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] core::FrameRate frameRate() const noexcept { return frameRate_; }
    [[nodiscard]] const std::vector<Track>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] const Track* track(const TrackId& id) const noexcept;

    [[nodiscard]] core::Result<void> addTrack(Track track);
    [[nodiscard]] core::Result<void> insertClip(const TrackId& trackId, Clip clip);
    [[nodiscard]] core::Result<void> replaceClip(
        const TrackId& trackId, const ClipId& clipId, Clip replacement);
    [[nodiscard]] core::Result<Clip> removeClip(
        const TrackId& trackId, const ClipId& clipId);

    friend bool operator==(const Timeline&, const Timeline&) = default;

private:
    Timeline(TimelineId id, std::string name, core::FrameRate frameRate)
        : id_(std::move(id)), name_(std::move(name)), frameRate_(frameRate) {}

    [[nodiscard]] Track* mutableTrack(const TrackId& id) noexcept;
    [[nodiscard]] bool containsClipId(const ClipId& id) const noexcept;
    [[nodiscard]] static core::Result<void> validateClips(
        TrackKind kind, const std::vector<Clip>& clips);

    TimelineId id_;
    std::string name_;
    core::FrameRate frameRate_;
    std::vector<Track> tracks_;
};

}  // namespace creator::domain
