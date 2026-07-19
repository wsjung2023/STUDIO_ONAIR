#pragma once

#include "capture/CaptureTimestampMapper.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace creator::capture {

/// Interleaved float32 samples copied from one native callback.
struct NativeAudioBlock final {
    NativeTimestamp timestamp;
    std::uint32_t sampleRate{0};
    std::uint32_t channels{0};
    std::uint32_t frameCount{0};
    std::size_t sampleCount{0};
    std::shared_ptr<float[]> samples;
};

/// Validates neutral PCM metadata and lazily maps one audio source's native PTS.
class AudioCaptureBlockAssembler final {
public:
    using ProjectAnchorProvider = std::function<creator::core::TimestampNs()>;

    AudioCaptureBlockAssembler();
    explicit AudioCaptureBlockAssembler(creator::core::TimestampNs projectAnchor) noexcept;
    explicit AudioCaptureBlockAssembler(ProjectAnchorProvider anchorProvider);

    [[nodiscard]] creator::core::Result<creator::media::AudioBlock> assemble(
        NativeAudioBlock block);

private:
    ProjectAnchorProvider anchorProvider_;
    std::optional<CaptureTimestampMapper> timestampMapper_;
};

}  // namespace creator::capture
