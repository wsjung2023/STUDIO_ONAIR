#pragma once

#include "core/Result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace creator::avatar::inochi2d {

/// Verified identity of the opt-in, source-built Inochi2D C-FFI runtime.
///
/// The returned library path is absolute and has passed manifest, target,
/// architecture, file-set, SHA-256, export, and import checks. This snapshot is
/// diagnostic metadata; production code consumes Inochi2dVerifiedRuntime.
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

/// A verified and loaded runtime whose manifest, notices, main DLL, and staged
/// dependency DLLs remain leased for the lifetime of this object.
class Inochi2dVerifiedRuntime final {
public:
    ~Inochi2dVerifiedRuntime();
    Inochi2dVerifiedRuntime(Inochi2dVerifiedRuntime&&) noexcept;
    Inochi2dVerifiedRuntime& operator=(Inochi2dVerifiedRuntime&&) noexcept;
    Inochi2dVerifiedRuntime(const Inochi2dVerifiedRuntime&) = delete;
    Inochi2dVerifiedRuntime& operator=(const Inochi2dVerifiedRuntime&) = delete;

    [[nodiscard]] const Inochi2dRuntimeInfo& info() const noexcept;
    [[nodiscard]] void* resolveSymbol(std::string_view name) const noexcept;

private:
    class Impl;
    explicit Inochi2dVerifiedRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class Inochi2dRuntimeManifest;
};

class Inochi2dRuntimeManifest final {
public:
    /// Diagnostic-only snapshot verification. Production loading must use
    /// openVerified() so verified files cannot be replaced before mapping.
    [[nodiscard]] static core::Result<Inochi2dRuntimeInfo> loadAndVerify(
        const std::filesystem::path& runtimeRoot);

    /// Leases every staged artifact, verifies it, maps the DLL with hardened
    /// search flags, resolves all required exports, and retains every lease.
    [[nodiscard]] static core::Result<Inochi2dVerifiedRuntime> openVerified(
        const std::filesystem::path& runtimeRoot);
};

}  // namespace creator::avatar::inochi2d
