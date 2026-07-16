#pragma once

#include "core/Result.h"
#include "media/MediaTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace creator::capture {

/// Fixed-capacity FIFO handoff for one native audio producer and an
/// application consumer. A full queue rejects visibly instead of dropping.
class BoundedAudioBlockQueue final {
public:
    explicit BoundedAudioBlockQueue(std::size_t capacity);

    BoundedAudioBlockQueue(const BoundedAudioBlockQueue&) = delete;
    BoundedAudioBlockQueue& operator=(const BoundedAudioBlockQueue&) = delete;
    BoundedAudioBlockQueue(BoundedAudioBlockQueue&&) = delete;
    BoundedAudioBlockQueue& operator=(BoundedAudioBlockQueue&&) = delete;

    [[nodiscard]] creator::core::Result<void> tryPush(
        creator::media::AudioBlock block);
    [[nodiscard]] std::optional<creator::media::AudioBlock> tryPop();
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t overruns() const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<creator::media::AudioBlock> blocks_;
    std::atomic<std::uint64_t> overruns_{0};
};

}  // namespace creator::capture
