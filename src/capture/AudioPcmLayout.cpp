#include "capture/AudioPcmLayout.h"

#include "core/AppError.h"

#include <limits>

namespace creator::capture {

core::Result<void> validateFloat32PcmLayout(const Float32PcmLayout& layout) {
    if (layout.channels == 0 || !layout.packed || layout.bigEndian) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Native audio must be packed little-endian float32 PCM"};
    }
    if (layout.channels > std::numeric_limits<std::uint32_t>::max() / sizeof(float)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Native audio channel count exceeds layout limits"};
    }
    const auto expectedBytes = layout.interleaved
                                   ? layout.channels * static_cast<std::uint32_t>(sizeof(float))
                                   : static_cast<std::uint32_t>(sizeof(float));
    if (layout.bytesPerFrame != expectedBytes) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Native audio frame stride is unsupported"};
    }
    return core::ok();
}

}  // namespace creator::capture
