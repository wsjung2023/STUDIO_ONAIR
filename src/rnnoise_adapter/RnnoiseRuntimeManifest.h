#pragma once

#include "core/Result.h"

#include <filesystem>

namespace creator::rnnoise_adapter {

/// Verifies the complete audited RNNoise prefix before the denoiser is used.
///
/// Mirrors mlt_adapter::verifyMltRuntimeManifest: the on-disk prefix must match
/// its own rnnoise-runtime-manifest.json exactly — approved identity (pinned
/// version, source commit, source-archive SHA-256, BSD-3-Clause, static
/// linking), every listed file present with the recorded SHA-256, the required
/// lib + header present, and no unexpected file, path traversal, reparse point
/// or forbidden/GPL artifact. RNNoise is statically linked, so this runs at
/// processor construction time to confirm the audited prefix that was linked is
/// still intact, rather than gating a runtime DLL load. The manifest itself is
/// the only file not listed in its own file set.
[[nodiscard]] core::Result<void> verifyRnnoiseRuntimeManifest(
    const std::filesystem::path& runtimeRoot);

}  // namespace creator::rnnoise_adapter
