#pragma once

#include "capture/BoundedAudioBlockQueue.h"
#include "capture/IAudioBlockSink.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace creator::capture {

struct AudioCaptureMailboxStats final {
    std::uint64_t receivedBlocks{0};
    std::uint64_t overruns{0};
};

/// Bounded native-callback handoff for one audio source's PREVIEW consumer
/// (level meter). Overflow drops and counts; only native capture errors are
/// surfaced, because the source behind it is shared with the recorder.
class AudioCaptureMailbox final : public IAudioBlockSink {
public:
    explicit AudioCaptureMailbox(std::size_t capacity);

    void onCaptureStarted() noexcept override;
    void onAudioBlock(creator::media::AudioBlock block) noexcept override;
    void onCaptureError(creator::core::AppError error) noexcept override;

    [[nodiscard]] bool takeStarted() noexcept;
    [[nodiscard]] std::optional<creator::media::AudioBlock> tryPop() noexcept;
    [[nodiscard]] std::optional<creator::core::AppError> takeError();
    [[nodiscard]] AudioCaptureMailboxStats stats() const noexcept;
    void clear() noexcept;

private:
    void publishFirstError(creator::core::AppError error) noexcept;

    BoundedAudioBlockQueue queue_;
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> receivedBlocks_{0};
    mutable std::mutex errorMutex_;
    std::optional<creator::core::AppError> error_;
};

}  // namespace creator::capture
