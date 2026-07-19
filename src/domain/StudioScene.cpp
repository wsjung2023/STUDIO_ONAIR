#include "domain/StudioScene.h"

#include "core/AppError.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;

constexpr std::int32_t kMaximumPosition = 1023;
constexpr std::size_t kMaximumNameCodePoints = 200;

bool isVideoRole(StudioSourceRole role) noexcept {
    return role == StudioSourceRole::Screen || role == StudioSourceRole::Camera ||
           role == StudioSourceRole::Avatar;
}

bool isKnownRole(StudioSourceRole role) noexcept {
    switch (role) {
    case StudioSourceRole::Screen:
    case StudioSourceRole::Camera:
    case StudioSourceRole::Microphone:
    case StudioSourceRole::SystemAudio:
    case StudioSourceRole::Avatar:
        return true;
    }
    return false;
}

bool validPosition(std::int32_t position) noexcept {
    return position >= 0 && position <= kMaximumPosition;
}

std::optional<std::size_t> utf8CodePointCount(std::string_view value) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7fU) {
            width = 1;
            codePoint = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            width = 2;
            codePoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            width = 3;
            codePoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            width = 4;
            codePoint = first & 0x07U;
        } else {
            return std::nullopt;
        }
        if (width > value.size() - index) return std::nullopt;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return std::nullopt;
            codePoint = (codePoint << 6U) | (continuation & 0x3fU);
        }
        const bool overlong =
            (width == 2 && codePoint < 0x80U) ||
            (width == 3 && codePoint < 0x800U) ||
            (width == 4 && codePoint < 0x10000U);
        if (overlong || (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
            codePoint > 0x10ffffU || codePoint == 0) {
            return std::nullopt;
        }
        index += width;
        ++count;
    }
    return count;
}

bool validName(std::string_view name) noexcept {
    const auto length = utf8CodePointCount(name);
    return length.has_value() && *length > 0 &&
           *length <= kMaximumNameCodePoints;
}

core::Result<VisualTransform> fullFrame(std::int32_t zOrder) {
    return VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, zOrder);
}

core::Result<VisualTransform> presentationCamera() {
    return VisualTransform::create(
        0.70, 0.05, 0.25, 0.25, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 10);
}

core::Result<SceneSource> makeDefaultSource(
    std::string id, StudioSourceRole role, std::string name,
    std::int32_t position, bool enabled,
    std::optional<VisualTransform> transform) {
    auto sourceId = SourceId::create(std::move(id));
    if (!sourceId.hasValue()) return sourceId.error();
    return SceneSource::create(std::move(sourceId).value(), role,
                               std::move(name), position, enabled,
                               std::move(transform));
}

core::Result<StudioScene> makeDefaultScene(
    std::string id, std::string name, std::int32_t position,
    const VisualTransform& screenTransform,
    const VisualTransform& cameraTransform, bool screenEnabled,
    bool cameraEnabled) {
    std::vector<SceneSource> sources;
    sources.reserve(4);
    for (auto candidate : {
             makeDefaultSource("screen", StudioSourceRole::Screen, "Screen", 0,
                               screenEnabled, screenTransform),
             makeDefaultSource("camera", StudioSourceRole::Camera, "Camera", 1,
                               cameraEnabled, cameraTransform),
             makeDefaultSource("microphone", StudioSourceRole::Microphone,
                               "Microphone", 2, true, std::nullopt),
             makeDefaultSource("system-audio", StudioSourceRole::SystemAudio,
                               "System Audio", 3, true, std::nullopt)}) {
        if (!candidate.hasValue()) return candidate.error();
        sources.push_back(std::move(candidate).value());
    }

    auto sceneId = SceneId::create(std::move(id));
    if (!sceneId.hasValue()) return sceneId.error();
    return StudioScene::create(std::move(sceneId).value(), std::move(name),
                               position, std::move(sources));
}

}  // namespace

std::string_view studioSourceRoleName(StudioSourceRole role) noexcept {
    switch (role) {
    case StudioSourceRole::Screen:
        return "screen";
    case StudioSourceRole::Camera:
        return "camera";
    case StudioSourceRole::Microphone:
        return "microphone";
    case StudioSourceRole::SystemAudio:
        return "system_audio";
    case StudioSourceRole::Avatar:
        return "avatar";
    }
    return {};
}

core::Result<StudioSourceRole> studioSourceRoleFromName(std::string_view name) {
    if (name == "screen") return StudioSourceRole::Screen;
    if (name == "camera") return StudioSourceRole::Camera;
    if (name == "microphone") return StudioSourceRole::Microphone;
    if (name == "system_audio") return StudioSourceRole::SystemAudio;
    if (name == "avatar") return StudioSourceRole::Avatar;
    return AppError{ErrorCode::InvalidArgument, "unknown studio source role"};
}

core::Result<SceneSource> SceneSource::create(
    SourceId id, StudioSourceRole role, std::string name,
    std::int32_t position, bool enabled,
    std::optional<VisualTransform> transform) {
    if (!isKnownRole(role) || !validName(name) || !validPosition(position)) {
        return AppError{ErrorCode::InvalidArgument,
                        "studio source is outside valid bounds"};
    }
    if (isVideoRole(role)) {
        if (enabled && !transform.has_value()) {
            return AppError{ErrorCode::InvalidArgument,
                            "enabled video source requires a visual transform"};
        }
    } else if (transform.has_value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "audio source cannot have a visual transform"};
    }
    return SceneSource{std::move(id), role, std::move(name), position, enabled,
                       std::move(transform)};
}

core::Result<SceneSource> SceneSource::withName(std::string name) const {
    return create(id_, role_, std::move(name), position_, enabled_, transform_);
}

core::Result<SceneSource> SceneSource::withPosition(
    std::int32_t position) const {
    return create(id_, role_, name_, position, enabled_, transform_);
}

core::Result<SceneSource> SceneSource::withEnabled(bool enabled) const {
    return create(id_, role_, name_, position_, enabled, transform_);
}

core::Result<SceneSource> SceneSource::withTransform(
    std::optional<VisualTransform> transform) const {
    return create(id_, role_, name_, position_, enabled_, std::move(transform));
}

core::Result<StudioScene> StudioScene::create(
    SceneId id, std::string name, std::int32_t position,
    std::vector<SceneSource> sources) {
    if (!validName(name) || !validPosition(position)) {
        return AppError{ErrorCode::InvalidArgument,
                        "studio scene is outside valid bounds"};
    }

    std::unordered_set<std::string> ids;
    std::unordered_set<std::int32_t> positions;
    std::array<bool, 5> roles{};
    for (const auto& source : sources) {
        const auto roleIndex = static_cast<std::size_t>(source.role());
        if (roleIndex >= roles.size() || roles[roleIndex] ||
            !ids.insert(source.id().value()).second ||
            !positions.insert(source.position()).second) {
            return AppError{ErrorCode::InvalidArgument,
                            "studio scene sources must be unique"};
        }
        roles[roleIndex] = true;
    }
    std::ranges::sort(sources, {}, &SceneSource::position);
    return StudioScene{std::move(id), std::move(name), position,
                       std::move(sources)};
}

core::Result<StudioScene> StudioScene::withName(std::string name) const {
    return create(id_, std::move(name), position_, sources_);
}

core::Result<StudioScene> StudioScene::withPosition(
    std::int32_t position) const {
    return create(id_, name_, position, sources_);
}

core::Result<StudioScene> StudioScene::withAddedSource(
    SceneSource source) const {
    auto sources = sources_;
    sources.push_back(std::move(source));
    return create(id_, name_, position_, std::move(sources));
}

core::Result<StudioScene> StudioScene::withoutSource(
    const SourceId& sourceId) const {
    auto sources = sources_;
    const auto removed = std::erase_if(
        sources, [&sourceId](const SceneSource& source) {
            return source.id() == sourceId;
        });
    if (removed != 1) {
        return AppError{ErrorCode::NotFound, "studio source was not found"};
    }
    return create(id_, name_, position_, std::move(sources));
}

core::Result<StudioScene> StudioScene::withSource(SceneSource source) const {
    auto sources = sources_;
    const auto existing = std::ranges::find(sources, source.id(),
                                            &SceneSource::id);
    if (existing == sources.end()) {
        return AppError{ErrorCode::NotFound, "studio source was not found"};
    }
    *existing = std::move(source);
    return create(id_, name_, position_, std::move(sources));
}

core::Result<StudioScene> StudioScene::withSourcePosition(
    const SourceId& sourceId, std::int32_t position) const {
    const auto existing = std::ranges::find(sources_, sourceId,
                                            &SceneSource::id);
    if (existing == sources_.end()) {
        return AppError{ErrorCode::NotFound, "studio source was not found"};
    }
    auto reordered = existing->withPosition(position);
    if (!reordered.hasValue()) return reordered.error();
    return withSource(std::move(reordered).value());
}

core::Result<std::vector<StudioScene>> defaultStudioScenes() {
    auto screen = fullFrame(0);
    auto cameraFull = fullFrame(10);
    auto cameraPip = presentationCamera();
    if (!screen.hasValue()) return screen.error();
    if (!cameraFull.hasValue()) return cameraFull.error();
    if (!cameraPip.hasValue()) return cameraPip.error();

    std::vector<StudioScene> scenes;
    scenes.reserve(3);
    for (auto candidate : {
             makeDefaultScene("presentation", "Presentation", 0,
                              screen.value(), cameraPip.value(), true, true),
             makeDefaultScene("screen", "Screen", 1, screen.value(),
                              cameraPip.value(), true, false),
             makeDefaultScene("camera", "Camera", 2, screen.value(),
                              cameraFull.value(), false, true)}) {
        if (!candidate.hasValue()) return candidate.error();
        scenes.push_back(std::move(candidate).value());
    }
    return scenes;
}

}  // namespace creator::domain
