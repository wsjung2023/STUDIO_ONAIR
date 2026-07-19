#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "core/Result.h"

#include <filesystem>
#include <memory>
#include <span>

namespace creator::avatar::inochi2d {

/// Optional runtime bridge for the official Inochi2D C FFI.
/// The SDK remains an external runtime dependency; required symbols are
/// resolved at startup and no model bytes are bundled in Creator Studio.
class Inochi2dModelRuntime final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<Inochi2dModelRuntime>> open(
        const std::filesystem::path& libraryPath,
        const std::filesystem::path& modelPath);

    ~Inochi2dModelRuntime();
    Inochi2dModelRuntime(const Inochi2dModelRuntime&) = delete;
    Inochi2dModelRuntime& operator=(const Inochi2dModelRuntime&) = delete;
    Inochi2dModelRuntime(Inochi2dModelRuntime&&) = delete;
    Inochi2dModelRuntime& operator=(Inochi2dModelRuntime&&) = delete;

    [[nodiscard]] core::Result<void> applyParameters(
        std::span<const AvatarParameterValue> parameters);
    [[nodiscard]] core::Result<void> update(float deltaSeconds);
    void close() noexcept;

private:
    class Impl;
    explicit Inochi2dModelRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::avatar::inochi2d
