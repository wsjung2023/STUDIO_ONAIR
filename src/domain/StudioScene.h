#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "domain/TimelineTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator::domain {

enum class StudioSourceRole {
    Screen,
    Camera,
    Microphone,
    SystemAudio,
};

[[nodiscard]] std::string_view studioSourceRoleName(
    StudioSourceRole role) noexcept;
[[nodiscard]] core::Result<StudioSourceRole> studioSourceRoleFromName(
    std::string_view name);

class SceneSource final {
public:
    [[nodiscard]] static core::Result<SceneSource> create(
        SourceId id, StudioSourceRole role, std::string name,
        std::int32_t position, bool enabled,
        std::optional<VisualTransform> transform);

    [[nodiscard]] const SourceId& id() const noexcept { return id_; }
    [[nodiscard]] StudioSourceRole role() const noexcept { return role_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::int32_t position() const noexcept { return position_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] const std::optional<VisualTransform>& transform() const noexcept {
        return transform_;
    }

    [[nodiscard]] core::Result<SceneSource> withName(std::string name) const;
    [[nodiscard]] core::Result<SceneSource> withPosition(
        std::int32_t position) const;
    [[nodiscard]] core::Result<SceneSource> withEnabled(bool enabled) const;
    [[nodiscard]] core::Result<SceneSource> withTransform(
        std::optional<VisualTransform> transform) const;

    friend bool operator==(const SceneSource&, const SceneSource&) = default;

private:
    SceneSource(SourceId id, StudioSourceRole role, std::string name,
                std::int32_t position, bool enabled,
                std::optional<VisualTransform> transform)
        : id_(std::move(id)),
          role_(role),
          name_(std::move(name)),
          position_(position),
          enabled_(enabled),
          transform_(std::move(transform)) {}

    SourceId id_;
    StudioSourceRole role_;
    std::string name_;
    std::int32_t position_;
    bool enabled_;
    std::optional<VisualTransform> transform_;
};

class StudioScene final {
public:
    [[nodiscard]] static core::Result<StudioScene> create(
        SceneId id, std::string name, std::int32_t position,
        std::vector<SceneSource> sources);

    [[nodiscard]] const SceneId& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::int32_t position() const noexcept { return position_; }
    [[nodiscard]] const std::vector<SceneSource>& sources() const noexcept {
        return sources_;
    }

    [[nodiscard]] core::Result<StudioScene> withName(std::string name) const;
    [[nodiscard]] core::Result<StudioScene> withPosition(
        std::int32_t position) const;
    [[nodiscard]] core::Result<StudioScene> withAddedSource(
        SceneSource source) const;
    [[nodiscard]] core::Result<StudioScene> withoutSource(
        const SourceId& sourceId) const;
    [[nodiscard]] core::Result<StudioScene> withSource(
        SceneSource source) const;
    [[nodiscard]] core::Result<StudioScene> withSourcePosition(
        const SourceId& sourceId, std::int32_t position) const;

    friend bool operator==(const StudioScene&, const StudioScene&) = default;

private:
    StudioScene(SceneId id, std::string name, std::int32_t position,
                std::vector<SceneSource> sources)
        : id_(std::move(id)),
          name_(std::move(name)),
          position_(position),
          sources_(std::move(sources)) {}

    SceneId id_;
    std::string name_;
    std::int32_t position_;
    std::vector<SceneSource> sources_;
};

[[nodiscard]] core::Result<std::vector<StudioScene>> defaultStudioScenes();

}  // namespace creator::domain
