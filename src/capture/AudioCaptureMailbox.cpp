#include "capture/AudioCaptureMailbox.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

AudioCaptureMailbox::AudioCaptureMailbox(std::size_t capacity) : queue_(capacity) {}

void AudioCaptureMailbox::onCaptureStarted() noexcept {
    started_.store(true, std::memory_order_release);
}

void AudioCaptureMailbox::onAudioBlock(creator::media::AudioBlock block) noexcept {
    // This queue feeds the preview level meter only. When the UI thread stalls
    // and the queue fills, the block is dropped and counted (queue overruns)
    // -- it is NOT an error: the capture source behind this mailbox is shared
    // with the recorder, and raising an error here used to stop that source
    // and silently end the live take's audio track.
    if (queue_.tryPush(std::move(block)) == AudioQueuePushResult::Full) return;
    receivedBlocks_.fetch_add(1, std::memory_order_relaxed);
}

void AudioCaptureMailbox::onCaptureError(creator::core::AppError error) noexcept {
    publishFirstError(std::move(error));
}

bool AudioCaptureMailbox::takeStarted() noexcept {
    return started_.exchange(false, std::memory_order_acq_rel);
}

std::optional<creator::media::AudioBlock> AudioCaptureMailbox::tryPop() noexcept {
    return queue_.tryPop();
}

std::optional<creator::core::AppError> AudioCaptureMailbox::takeError() {
    std::scoped_lock lock{errorMutex_};
    auto pending = std::move(error_);
    error_.reset();
    return pending;
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

void AudioCaptureMailbox::publishFirstError(creator::core::AppError error) noexcept {
    try {
        std::scoped_lock lock{errorMutex_};
        if (!error_) {
            error_.emplace(std::move(error));
        }
    } catch (...) {
        // AppError moves existing storage only; this guard prevents any
        // implementation-specific mutex failure from crossing a native callback.
    }
}

}  // namespace creator::capture
