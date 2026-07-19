#include "capture/CaptureFanoutSinks.h"

#include <utility>

namespace creator::capture {
namespace {

// MSVC ships the C++20 atomic<shared_ptr> specialization; the Android NDK
// libc++ used by this product does not. The pre-C++20 shared_ptr atomic free
// functions keep the same acquire/release contract on NDK without relying on
// the unavailable specialization.
template <typename Sink>
void storeSecondary(
#if defined(_MSC_VER)
    std::atomic<std::shared_ptr<Sink>>* target,
#else
    std::shared_ptr<Sink>* target,
#endif
    std::shared_ptr<Sink> secondary) noexcept {
#if defined(_MSC_VER)
    target->store(std::move(secondary), std::memory_order_release);
#else
    std::atomic_store_explicit(target, std::move(secondary), std::memory_order_release);
#endif
}

template <typename Sink>
std::shared_ptr<Sink> loadSecondary(
#if defined(_MSC_VER)
    const std::atomic<std::shared_ptr<Sink>>* target
#else
    const std::shared_ptr<Sink>* target
#endif
) noexcept {
#if defined(_MSC_VER)
    return target->load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(target, std::memory_order_acquire);
#endif
}

}  // namespace

VideoFrameFanoutSink::VideoFrameFanoutSink(std::shared_ptr<IVideoFrameSink> primary)
    : primary_(std::move(primary)) {}

void VideoFrameFanoutSink::setSecondary(
    std::shared_ptr<IVideoFrameSink> secondary) noexcept {
    storeSecondary(&secondary_, std::move(secondary));
}

void VideoFrameFanoutSink::onCaptureStarted() noexcept {
    const auto secondary = loadSecondary(&secondary_);
    if (primary_) primary_->onCaptureStarted();
    if (secondary && secondary != primary_) secondary->onCaptureStarted();
}

void VideoFrameFanoutSink::onVideoFrame(media::VideoFrame frame) noexcept {
    const auto secondary = loadSecondary(&secondary_);
    if (primary_) primary_->onVideoFrame(frame);
    if (secondary && secondary != primary_) secondary->onVideoFrame(std::move(frame));
}

void VideoFrameFanoutSink::onCaptureError(core::AppError error) noexcept {
    const auto secondary = loadSecondary(&secondary_);
    if (primary_) primary_->onCaptureError(error);
    if (secondary && secondary != primary_) secondary->onCaptureError(std::move(error));
}

AudioBlockFanoutSink::AudioBlockFanoutSink(std::shared_ptr<IAudioBlockSink> primary)
    : primary_(std::move(primary)) {}

void AudioBlockFanoutSink::setSecondary(
    std::shared_ptr<IAudioBlockSink> secondary) noexcept {
    storeSecondary(&secondary_, std::move(secondary));
}

void AudioBlockFanoutSink::onCaptureStarted() noexcept {
    const auto secondary = loadSecondary(&secondary_);
    if (primary_) primary_->onCaptureStarted();
    if (secondary && secondary != primary_) secondary->onCaptureStarted();
}

void AudioBlockFanoutSink::onAudioBlock(media::AudioBlock block) noexcept {
    const auto secondary = loadSecondary(&secondary_);
    if (primary_) primary_->onAudioBlock(block);
    if (secondary && secondary != primary_) secondary->onAudioBlock(std::move(block));
}

void AudioBlockFanoutSink::onCaptureError(core::AppError error) noexcept {
    const auto secondary = loadSecondary(&secondary_);
    if (primary_) primary_->onCaptureError(error);
    if (secondary && secondary != primary_) secondary->onCaptureError(std::move(error));
}

}  // namespace creator::capture
