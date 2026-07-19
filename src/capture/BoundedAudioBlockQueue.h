#pragma once

#include "media/MediaTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace creator::capture {

enum class AudioQueuePushResult { Accepted, Full };

/// Fixed-capacity FIFO handoff for one native audio producer and an
/// application consumer. A full queue rejects visibly instead of dropping.
class BoundedAudioBlockQueue final {
public:
    explicit BoundedAudioBlockQueue(std::size_t capacity);

    BoundedAudioBlockQueue(const BoundedAudioBlockQueue&) = delete;
    BoundedAudioBlockQueue& operator=(const BoundedAudioBlockQueue&) = delete;
    BoundedAudioBlockQueue(BoundedAudioBlockQueue&&) = delete;
    BoundedAudioBlockQueue& operator=(BoundedAudioBlockQueue&&) = delete;

    [[nodiscard]] AudioQueuePushResult tryPush(creator::media::AudioBlock block) noexcept;
    [[nodiscard]] std::optional<creator::media::AudioBlock> tryPop() noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t overruns() const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t capacity_;
    std::vector<std::optional<creator::media::AudioBlock>> slots_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::uint64_t> overruns_{0};
};

}  // namespace creator::capture
