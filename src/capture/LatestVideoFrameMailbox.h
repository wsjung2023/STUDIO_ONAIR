#pragma once

#include "capture/IVideoFrameSink.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace creator::capture {

struct LatestVideoFrameMailboxStats final {
    std::uint64_t publishedFrames{0};
    std::uint64_t replacedFrames{0};
    std::uint64_t terminalErrors{0};
    std::uint64_t framesAfterTerminalError{0};
    std::uint32_t lastWidth{0};
    std::uint32_t lastHeight{0};

    friend bool operator==(const LatestVideoFrameMailboxStats&,
                           const LatestVideoFrameMailboxStats&) = default;
};

/// A bounded, non-blocking handoff from a capture callback to preview.
///
/// The producer owns exactly one pending slot. Publishing while it is occupied
/// replaces the older preview frame and releases its platform handle. This is
/// intentionally different from R0-05's recording queue: preview latency wins
/// over preserving every frame, and memory use never grows with UI delay.
class LatestVideoFrameMailbox final : public IVideoFrameSink {
public:
    LatestVideoFrameMailbox() = default;

    void onVideoFrame(creator::media::VideoFrame frame) noexcept override;
    void onCaptureError(creator::core::AppError error) noexcept override;

    [[nodiscard]] std::optional<creator::media::VideoFrame> takeLatest();
    [[nodiscard]] std::optional<creator::core::AppError> takeError();
    [[nodiscard]] LatestVideoFrameMailboxStats stats() const noexcept;

private:
    mutable std::mutex mutex_;
    std::optional<creator::media::VideoFrame> pendingFrame_;
    std::optional<creator::core::AppError> pendingError_;
    bool terminal_{false};
    LatestVideoFrameMailboxStats stats_;
};

}  // namespace creator::capture
