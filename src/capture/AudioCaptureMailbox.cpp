#include "capture/AudioCaptureMailbox.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

AudioCaptureMailbox::AudioCaptureMailbox(std::size_t capacity) : queue_(capacity) {}

void AudioCaptureMailbox::onCaptureStarted() noexcept {
    started_.store(true, std::memory_order_release);
}

void AudioCaptureMailbox::onAudioBlock(creator::media::AudioBlock block) noexcept {
    if (queue_.tryPush(std::move(block)) == AudioQueuePushResult::Full) {
        publishFirstError(PendingError{ErrorKind::Overflow, std::nullopt});
        return;
    }
    receivedBlocks_.fetch_add(1, std::memory_order_relaxed);
}

void AudioCaptureMailbox::onCaptureError(creator::core::AppError error) noexcept {
    publishFirstError(PendingError{ErrorKind::Native, std::move(error)});
}

bool AudioCaptureMailbox::takeStarted() noexcept {
    return started_.exchange(false, std::memory_order_acq_rel);
}

std::optional<creator::media::AudioBlock> AudioCaptureMailbox::tryPop() noexcept {
    return queue_.tryPop();
}

std::optional<creator::core::AppError> AudioCaptureMailbox::takeError() {
    std::optional<PendingError> pending;
    {
        std::scoped_lock lock{errorMutex_};
        pending = std::move(error_);
        error_.reset();
    }
    if (!pending) {
        return std::nullopt;
    }
    if (pending->kind == ErrorKind::Native) {
        return std::move(pending->native);
    }
    return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                   "audio capture queue capacity exceeded"};
}

AudioCaptureMailboxStats AudioCaptureMailbox::stats() const noexcept {
    return {receivedBlocks_.load(std::memory_order_relaxed), queue_.overruns()};
}

void AudioCaptureMailbox::clear() noexcept {
    queue_.clear();
    started_.store(false, std::memory_order_relaxed);
    std::scoped_lock lock{errorMutex_};
    error_.reset();
}

void AudioCaptureMailbox::publishFirstError(PendingError error) noexcept {
    try {
        std::scoped_lock lock{errorMutex_};
        if (!error_) {
            error_.emplace(std::move(error));
        }
    } catch (...) {
        // PendingError moves existing storage only; this guard prevents any
        // implementation-specific mutex failure from crossing a native callback.
    }
}

}  // namespace creator::capture
