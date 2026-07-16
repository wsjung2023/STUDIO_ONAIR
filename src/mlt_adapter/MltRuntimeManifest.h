#pragma once

#include "core/Result.h"

#include <filesystem>

namespace creator::mlt_adapter {

/// Verifies the complete audited MLT prefix before the MLT factory can load a
/// DLL or discover a module. The manifest itself is the only file not listed
/// in its own file set.
[[nodiscard]] core::Result<void> verifyMltRuntimeManifest(
    const std::filesystem::path& runtimeRoot);

}  // namespace creator::mlt_adapter
