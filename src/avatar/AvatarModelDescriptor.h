#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace creator::avatar {

/// Validated sidecar description for an avatar model package.
///
/// The descriptor keeps file/package policy out of the eventual Inochi2D
/// adapter. Its `model` path is resolved relative to the descriptor and must
/// remain inside that directory; parameter names and ranges are passed through
/// AvatarParameterMapper's validation boundary.
class AvatarModelDescriptor final {
public:
    [[nodiscard]] static core::Result<AvatarModelDescriptor> load(
        const std::filesystem::path& descriptorPath);

    [[nodiscard]] const std::filesystem::path& descriptorPath() const noexcept {
        return descriptorPath_;
    }
    [[nodiscard]] const std::filesystem::path& modelPath() const noexcept {
        return modelPath_;
    }
    [[nodiscard]] const std::string& renderer() const noexcept { return renderer_; }
    [[nodiscard]] std::uint32_t canvasWidth() const noexcept { return canvasWidth_; }
    [[nodiscard]] std::uint32_t canvasHeight() const noexcept { return canvasHeight_; }
    [[nodiscard]] const AvatarParameterMapper& parameterMapper() const noexcept {
        return parameterMapper_;
    }

private:
    AvatarModelDescriptor(std::filesystem::path descriptorPath,
                          std::filesystem::path modelPath, std::string renderer,
                          std::uint32_t canvasWidth, std::uint32_t canvasHeight,
                          AvatarParameterMapper parameterMapper)
        : descriptorPath_(std::move(descriptorPath)), modelPath_(std::move(modelPath)),
          renderer_(std::move(renderer)), canvasWidth_(canvasWidth),
          canvasHeight_(canvasHeight), parameterMapper_(std::move(parameterMapper)) {}

    std::filesystem::path descriptorPath_;
    std::filesystem::path modelPath_;
    std::string renderer_;
    std::uint32_t canvasWidth_;
    std::uint32_t canvasHeight_;
    AvatarParameterMapper parameterMapper_;
};

}  // namespace creator::avatar
