#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace creator::recorder {

enum class TrackMediaKind { Video, Audio };
enum class TrackRole {
    Screen,
    Camera,
    Avatar,
    Microphone,
    SystemAudio,
    CompositePreview
};

class RecordingTrack final {
public:
    [[nodiscard]] static core::Result<RecordingTrack> create(
        domain::SourceId sourceId, TrackRole role);

    [[nodiscard]] const domain::SourceId& sourceId() const noexcept { return sourceId_; }
    [[nodiscard]] TrackRole role() const noexcept { return role_; }
    [[nodiscard]] TrackMediaKind mediaKind() const noexcept { return mediaKind_; }
    [[nodiscard]] const std::string& pathComponent() const noexcept { return pathComponent_; }

    friend bool operator==(const RecordingTrack&, const RecordingTrack&) = default;

private:
    RecordingTrack(domain::SourceId sourceId, TrackRole role, TrackMediaKind mediaKind,
                   std::string pathComponent);

    domain::SourceId sourceId_;
    TrackRole role_;
    TrackMediaKind mediaKind_;
    std::string pathComponent_;
};

[[nodiscard]] core::Result<std::string> safeSourcePathComponent(
    const domain::SourceId& sourceId);
/// Percent-encodes a recording session id into one portable path component.
/// Every take owns its own segment directory beneath the source directory, so
/// a second take of the same source in the same project never collides with
/// (or overwrites) the first take's files.
[[nodiscard]] core::Result<std::string> safeTakePathComponent(
    const domain::SessionId& sessionId);
[[nodiscard]] std::filesystem::path relativeSegmentPath(const RecordingTrack& track,
                                                        std::string_view takeComponent,
                                                        std::uint64_t index);
[[nodiscard]] std::filesystem::path temporarySegmentPath(const RecordingTrack& track,
                                                         std::string_view takeComponent,
                                                         std::uint64_t index);

}  // namespace creator::recorder
