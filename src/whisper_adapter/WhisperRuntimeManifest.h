#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string>

namespace creator::whisper_adapter {

/// Identity of the audited whisper.cpp runtime resolved from its manifest.
///
/// Responsibility: carry the two things the provider needs after verification -
/// the absolute, on-disk-verified path of the pinned ggml model and its
/// SHA-256 - so the caller never re-derives a path the manifest already proved.
struct WhisperRuntimeInfo final {
    std::filesystem::path modelPath;
    std::string modelSha256;
};

/// Verifies the audited whisper.cpp install prefix before any library symbol or
/// model file is loaded. Mirrors mlt_adapter::verifyMltRuntimeManifest: it reads
/// whisper-runtime-manifest.json, checks the pinned identity (version, source
/// commit, model name/hash) against the values compiled into this adapter,
/// re-hashes every file the manifest lists and rejects any mismatch, any GPL or
/// otherwise non-MIT provenance, and any path that escapes the runtime root.
///
/// Unlike the frozen MLT tree, whisper's installer emits a backend-dependent DLL
/// set, so this does not assert an exact whole-directory file set; it pins the
/// model exactly and verifies the hash of every artifact the bootstrap recorded.
///
/// Returns the verified model location on success, or an AppError (never throws).
[[nodiscard]] core::Result<WhisperRuntimeInfo> verifyWhisperRuntimeManifest(
    const std::filesystem::path& runtimeRoot);

}  // namespace creator::whisper_adapter
