#include "capture/BoundedAudioBlockQueue.h"

#include <utility>

namespace creator::capture {

BoundedAudioBlockQueue::BoundedAudioBlockQueue(std::size_t capacity)
    : capacity_(capacity), slots_(capacity + 1) {}

AudioQueuePushResult BoundedAudioBlockQueue::tryPush(
    creator::media::AudioBlock block) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto next = (tail + 1) % slots_.size();
    if (next == head_.load(std::memory_order_acquire)) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        return AudioQueuePushResult::Full;
    }
    slots_[tail].emplace(std::move(block));
    tail_.store(next, std::memory_order_release);
    return AudioQueuePushResult::Accepted;
}

std::optional<creator::media::AudioBlock> BoundedAudioBlockQueue::tryPop() noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    auto block = std::move(slots_[head]);
    slots_[head].reset();
    head_.store((head + 1) % slots_.size(), std::memory_order_release);
    return block;
}

void BoundedAudioBlockQueue::clear() noexcept {
    while (tryPop().has_value()) {
    }
}

std::size_t BoundedAudioBlockQueue::size() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_acquire);
    return tail >= head ? tail - head : slots_.size() - head + tail;
}

}  // namespace creator::capture
