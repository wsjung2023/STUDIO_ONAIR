#pragma once

#include "core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace creator::avatar::vrm {

/// The two payloads of a binary glTF (.glb / .vrm) container: the glTF JSON
/// document and the single binary buffer (BIN chunk) its accessors index into.
struct GlbContainer final {
    std::string json;
    std::vector<std::byte> bin;

    /// Parses a binary glTF container. VRM 0.x and 1.0 files are glb files, so
    /// this reads both. Every length is validated against the remaining bytes
    /// before use, so a truncated or hostile file is rejected, never over-read.
    ///
    /// Accepts the 12-byte header (magic `glTF`, version 2, total length) followed
    /// by chunks; the first chunk must be JSON, an optional second chunk is BIN.
    /// Extra chunk types are skipped. A plain-text `.gltf` (no glb header) is
    /// rejected here -- callers that want it must read the JSON directly.
    [[nodiscard]] static core::Result<GlbContainer> read(
        std::span<const std::byte> bytes);
};

}  // namespace creator::avatar::vrm
