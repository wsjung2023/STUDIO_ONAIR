#pragma once

#include "core/Result.h"

#include <cstdint>

namespace creator::capture {

struct Float32PcmLayout final {
    std::uint32_t channels{0};
    std::uint32_t bytesPerFrame{0};
    bool interleaved{true};
    bool packed{false};
    bool bigEndian{false};
};

/// Validates the exact native float32 layouts copied by the capture adapter.
[[nodiscard]] core::Result<void> validateFloat32PcmLayout(
    const Float32PcmLayout& layout);

}  // namespace creator::capture
