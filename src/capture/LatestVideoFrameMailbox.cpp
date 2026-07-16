#include "capture/LatestVideoFrameMailbox.h"

#include <utility>

namespace creator::capture {

void LatestVideoFrameMailbox::onVideoFrame(media::VideoFrame frame) noexcept {
    std::scoped_lock lock{mutex_};
    if (terminal_) {
        ++stats_.framesAfterTerminalError;
        return;
    }
    ++stats_.publishedFrames;
    stats_.lastWidth = frame.width;
    stats_.lastHeight = frame.height;
    if (pendingFrame_) {
        ++stats_.replacedFrames;
    }
    pendingFrame_ = std::move(frame);
}

void LatestVideoFrameMailbox::onCaptureError(core::AppError error) noexcept {
    std::scoped_lock lock{mutex_};
    ++stats_.terminalErrors;
    if (terminal_) {
        return;
    }
    terminal_ = true;
    pendingFrame_.reset();
    pendingError_ = std::move(error);
}

std::optional<media::VideoFrame> LatestVideoFrameMailbox::takeLatest() {
    std::scoped_lock lock{mutex_};
    auto frame = std::move(pendingFrame_);
    pendingFrame_.reset();
    return frame;
}

std::optional<core::AppError> LatestVideoFrameMailbox::takeError() {
    std::scoped_lock lock{mutex_};
    auto error = std::move(pendingError_);
    pendingError_.reset();
    return error;
}

LatestVideoFrameMailboxStats LatestVideoFrameMailbox::stats() const noexcept {
    std::scoped_lock lock{mutex_};
    return stats_;
}

}  // namespace creator::capture
