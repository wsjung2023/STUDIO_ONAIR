#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace creator::avatar::inochi2d {

/// Verified identity of the opt-in, source-built Inochi2D C-FFI runtime.
///
/// The returned library path is absolute and has already passed manifest,
/// target, architecture, file-set and SHA-256 checks. This type never loads the
/// library; callers must retain that verify-before-load ordering.
struct Inochi2dRuntimeInfo final {
    std::filesystem::path libraryPath;
    std::string librarySha256;
    std::string version;
    std::string sourceCommit;
    std::string target;
    std::string targetTriple;
    std::string minimumPlatform;
    std::string compilerIdentity;
    std::string sdkIdentity;
    std::vector<std::string> requiredSymbols;
};

class Inochi2dRuntimeManifest final {
public:
    /// Verifies runtime-manifest.json and the complete staged prefix without
    /// calling LoadLibraryW or dlopen.
    [[nodiscard]] static core::Result<Inochi2dRuntimeInfo> loadAndVerify(
        const std::filesystem::path& runtimeRoot);
};

}  // namespace creator::avatar::inochi2d
