#include "recorder/RecordingTrack.h"

#include "core/AppError.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace creator::recorder {
namespace {

constexpr std::size_t kMaxPathComponentBytes = 128;

TrackMediaKind mediaKindForRole(TrackRole role) noexcept {
    switch (role) {
    case TrackRole::Screen:
    case TrackRole::Camera:
    case TrackRole::Avatar:
    case TrackRole::CompositePreview:
        return TrackMediaKind::Video;
    case TrackRole::Microphone:
    case TrackRole::SystemAudio:
        return TrackMediaKind::Audio;
    }
    return TrackMediaKind::Video;
}

std::string_view roleDirectory(TrackRole role) noexcept {
    switch (role) {
    case TrackRole::Screen: return "media/screen";
    case TrackRole::Camera: return "media/camera";
    case TrackRole::Avatar: return "media/avatar";
    case TrackRole::Microphone: return "audio/microphone";
    case TrackRole::SystemAudio: return "audio/system";
    case TrackRole::CompositePreview: return "media/preview";
    }
    return "media/unknown";
}

bool portableUnescaped(unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-';
}

std::string segmentFilename(std::uint64_t index, std::string_view extension) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "segment_%06llu.%s",
                                      static_cast<unsigned long long>(index), extension.data());
    return written > 0 ? std::string{buffer.data(), static_cast<std::size_t>(written)}
                       : std::string{};
}

core::Result<std::string> safeIdentifierPathComponent(std::string_view identifier) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char value : identifier) {
        if (portableUnescaped(value)) {
            encoded.push_back(static_cast<char>(value));
        } else {
            // FFmpeg's concat demuxer accepts letters, digits, '-' and '_'
            // with safe=1. Escape '_' as well so this _HH form remains
            // injective ("/" and the literal text "_2F" cannot collide).
            encoded.push_back('_');
            encoded.push_back(hex[value >> 4U]);
            encoded.push_back(hex[value & 0x0FU]);
        }
        if (encoded.size() > kMaxPathComponentBytes) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Encoded identifier is too long for a media path"};
        }
    }
    return encoded;
}

}  // namespace

RecordingTrack::RecordingTrack(domain::SourceId sourceId, TrackRole role,
                               TrackMediaKind mediaKindValue, std::string pathComponent)
    : sourceId_(std::move(sourceId)),
      role_(role),
      mediaKind_(mediaKindValue),
      pathComponent_(std::move(pathComponent)) {}

core::Result<RecordingTrack> RecordingTrack::create(domain::SourceId sourceId,
                                                    TrackRole role) {
    auto component = safeSourcePathComponent(sourceId);
    if (!component.hasValue()) return component.error();
    return RecordingTrack{std::move(sourceId), role, mediaKindForRole(role),
                          std::move(component).value()};
}

core::Result<std::string> safeSourcePathComponent(const domain::SourceId& sourceId) {
    return safeIdentifierPathComponent(sourceId.value());
}

core::Result<std::string> safeSessionPathComponent(
    const domain::SessionId& sessionId) {
    return safeIdentifierPathComponent(sessionId.value());
}

std::filesystem::path relativeSegmentPath(const RecordingTrack& track,
                                          std::uint64_t index,
                                          SegmentContainer container) {
    const auto extension = container == SegmentContainer::Mp4
                               ? (track.mediaKind() == TrackMediaKind::Video ? "mp4"
                                                                            : "m4a")
                               : (track.mediaKind() == TrackMediaKind::Video ? "mkv"
                                                                            : "mka");
    return std::filesystem::path{roleDirectory(track.role())} / track.pathComponent() /
           segmentFilename(index, extension);
}

std::filesystem::path temporarySegmentPath(const RecordingTrack& track,
                                           std::uint64_t index,
                                           SegmentContainer container) {
    auto path = std::filesystem::path{".tmp"} /
                relativeSegmentPath(track, index, container);
    path += ".part";
    return path;
}

}  // namespace creator::recorder
