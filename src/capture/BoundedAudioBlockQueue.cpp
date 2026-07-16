#include "capture/BoundedAudioBlockQueue.h"

#include "core/AppError.h"

#include <cassert>
#include <utility>

namespace creator::capture {

BoundedAudioBlockQueue::BoundedAudioBlockQueue(std::size_t capacity) : capacity_(capacity) {
    assert(capacity_ > 0 && "audio queue capacity must be positive");
}

creator::core::Result<void> BoundedAudioBlockQueue::tryPush(
    creator::media::AudioBlock block) {
    std::scoped_lock lock{mutex_};
    if (blocks_.size() >= capacity_) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "audio capture queue capacity exceeded"};
    }
    blocks_.push_back(std::move(block));
    return creator::core::ok();
}

std::optional<creator::media::AudioBlock> BoundedAudioBlockQueue::tryPop() {
    std::scoped_lock lock{mutex_};
    if (blocks_.empty()) {
        return std::nullopt;
    }
    auto block = std::move(blocks_.front());
    blocks_.pop_front();
    return block;
}

void BoundedAudioBlockQueue::clear() noexcept {
    std::scoped_lock lock{mutex_};
    blocks_.clear();
}

std::size_t BoundedAudioBlockQueue::size() const noexcept {
    std::scoped_lock lock{mutex_};
    return blocks_.size();
}

}  // namespace creator::capture
