#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace creator::avatar::inochi2d {

struct Inochi2dBinaryInfo final {
    std::vector<std::string> exports;
    std::vector<std::string> imports;
};

/// Parses a PE32+ x64 DLL without mapping or executing it.
[[nodiscard]] core::Result<Inochi2dBinaryInfo> inspectWindowsX64Dll(
    const std::filesystem::path& path);

}  // namespace creator::avatar::inochi2d
