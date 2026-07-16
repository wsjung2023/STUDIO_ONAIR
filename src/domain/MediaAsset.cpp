#include "domain/MediaAsset.h"

#include "core/AppError.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;

core::Result<std::string> normalizeRelativePath(std::string_view input) {
    if (input.empty() || input.find('\0') != std::string_view::npos) {
        return AppError{ErrorCode::InvalidArgument, "asset path must be non-empty"};
    }

    std::string portable{input};
    std::replace(portable.begin(), portable.end(), '\\', '/');
    const bool drivePath = portable.size() >= 2 &&
                           std::isalpha(static_cast<unsigned char>(portable[0])) != 0 &&
                           portable[1] == ':';
    if (portable.front() == '/' || drivePath) {
        return AppError{ErrorCode::InvalidArgument, "asset path must be package-relative"};
    }

    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin <= portable.size()) {
        const auto end = portable.find('/', begin);
        const auto length = end == std::string::npos ? portable.size() - begin : end - begin;
        const std::string component = portable.substr(begin, length);
        if (component == "..") {
            return AppError{ErrorCode::InvalidArgument, "asset path must not escape the package"};
        }
        if (!component.empty() && component != ".") {
            components.push_back(component);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (components.empty()) {
        return AppError{ErrorCode::InvalidArgument, "asset path must name a file"};
    }

    std::string normalized;
    for (const auto& component : components) {
        if (!normalized.empty()) normalized.push_back('/');
        normalized += component;
    }
    return normalized;
}

bool validVideo(const VideoAssetMetadata& metadata) noexcept {
    return metadata.width > 0 && metadata.height > 0;
}

bool validAudio(const AudioAssetMetadata& metadata) noexcept {
    return metadata.sampleRate > 0 && metadata.channels > 0;
}

}  // namespace

core::Result<MediaAsset> MediaAsset::create(
    AssetId id, MediaKind kind, std::string_view relativePath,
    core::DurationNs duration, std::optional<VideoAssetMetadata> video,
    std::optional<AudioAssetMetadata> audio, std::uint64_t fileSize,
    std::string fingerprint, AssetAvailability availability) {
    auto normalizedPath = normalizeRelativePath(relativePath);
    if (!normalizedPath.hasValue()) return normalizedPath.error();
    if (duration <= core::DurationNs::zero() || fileSize == 0 || fingerprint.empty()) {
        return AppError{ErrorCode::InvalidArgument,
                        "asset duration, size, and fingerprint must be present"};
    }
    if (video.has_value() && !validVideo(*video)) {
        return AppError{ErrorCode::InvalidArgument, "asset video metadata is invalid"};
    }
    if (audio.has_value() && !validAudio(*audio)) {
        return AppError{ErrorCode::InvalidArgument, "asset audio metadata is invalid"};
    }
    if ((kind == MediaKind::Video && !video.has_value()) ||
        (kind == MediaKind::Audio && (!audio.has_value() || video.has_value())) ||
        (kind == MediaKind::Image && (!video.has_value() || audio.has_value()))) {
        return AppError{ErrorCode::InvalidArgument,
                        "asset metadata does not match its media kind"};
    }
    return MediaAsset{std::move(id), kind, std::move(normalizedPath).value(), duration,
                      std::move(video), std::move(audio), fileSize,
                      std::move(fingerprint), availability};
}

}  // namespace creator::domain
