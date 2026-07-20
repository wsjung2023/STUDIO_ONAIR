#pragma once

#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace creator::platform_release {

struct ModelInstallRequest final {
    std::filesystem::path finalPath;
    std::uint64_t downloadBytes{};
    std::string expectedSha256;
};

struct ModelStorageSnapshot final {
    std::uint64_t availableBytes{};
    std::uint64_t installedModelBytes{};
};

struct ModelInstallPlan final {
    std::filesystem::path stagingPath;
    std::filesystem::path finalPath;
    std::string expectedSha256;
    std::uint64_t expectedBytes{};
};

/// Validates admission before a model download starts. Publication code uses
/// the returned sibling `.part` path and must verify expectedSha256 before the
/// final atomic rename.
class ModelStoragePolicy final {
public:
    ModelStoragePolicy(std::filesystem::path modelRoot,
                       std::uint64_t maximumModelBytes)
        : modelRoot_(std::move(modelRoot)),
          maximumModelBytes_(maximumModelBytes) {}

    [[nodiscard]] core::Result<ModelInstallPlan> admit(
        const ModelInstallRequest& request,
        const ModelStorageSnapshot& storage) const;
    [[nodiscard]] core::Result<void> publishVerified(
        const ModelInstallPlan& plan) const;

private:
    std::filesystem::path modelRoot_;
    std::uint64_t maximumModelBytes_{};
};

}  // namespace creator::platform_release
