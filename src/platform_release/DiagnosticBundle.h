#pragma once

#include "core/Result.h"

#include <filesystem>
#include <vector>

namespace creator::platform_release {

struct DiagnosticBundleRequest final {
    std::filesystem::path sourceRoot;
    std::filesystem::path destination;
    std::vector<std::filesystem::path> files;
    bool consentGranted{false};
};

/// Creates a local-only, allowlisted diagnostic directory. This class has no
/// network dependency and therefore cannot upload the resulting bundle.
class DiagnosticBundle final {
public:
    [[nodiscard]] static core::Result<std::filesystem::path> create(
        const DiagnosticBundleRequest& request);
};

}  // namespace creator::platform_release
