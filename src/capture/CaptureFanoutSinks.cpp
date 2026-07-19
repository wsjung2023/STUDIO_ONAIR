#include "capture/CaptureFanoutSinks.h"

#include <utility>

namespace creator::capture {

VideoFrameFanoutSink::VideoFrameFanoutSink(std::shared_ptr<IVideoFrameSink> primary)
    : primary_(std::move(primary)) {}

void VideoFrameFanoutSink::setSecondary(
    std::shared_ptr<IVideoFrameSink> secondary) noexcept {
    secondary_.store(std::move(secondary), std::memory_order_release);
}

void VideoFrameFanoutSink::onCaptureStarted() noexcept {
    const auto secondary = secondary_.load(std::memory_order_acquire);
    if (primary_) primary_->onCaptureStarted();
    if (secondary && secondary != primary_) secondary->onCaptureStarted();
}

void VideoFrameFanoutSink::onVideoFrame(media::VideoFrame frame) noexcept {
    const auto secondary = secondary_.load(std::memory_order_acquire);
    if (primary_) primary_->onVideoFrame(frame);
    if (secondary && secondary != primary_) secondary->onVideoFrame(std::move(frame));
}

void VideoFrameFanoutSink::onCaptureError(core::AppError error) noexcept {
    const auto secondary = secondary_.load(std::memory_order_acquire);
    if (primary_) primary_->onCaptureError(error);
    if (secondary && secondary != primary_) secondary->onCaptureError(std::move(error));
}

AudioBlockFanoutSink::AudioBlockFanoutSink(std::shared_ptr<IAudioBlockSink> primary)
    : primary_(std::move(primary)) {}

void AudioBlockFanoutSink::setSecondary(
    std::shared_ptr<IAudioBlockSink> secondary) noexcept {
    secondary_.store(std::move(secondary), std::memory_order_release);
}

void AudioBlockFanoutSink::onCaptureStarted() noexcept {
    const auto secondary = secondary_.load(std::memory_order_acquire);
    if (primary_) primary_->onCaptureStarted();
    if (secondary && secondary != primary_) secondary->onCaptureStarted();
}

void AudioBlockFanoutSink::onAudioBlock(media::AudioBlock block) noexcept {
    const auto secondary = secondary_.load(std::memory_order_acquire);
    if (primary_) primary_->onAudioBlock(block);
    if (secondary && secondary != primary_) secondary->onAudioBlock(std::move(block));
}

void AudioBlockFanoutSink::onCaptureError(core::AppError error) noexcept {
    const auto secondary = secondary_.load(std::memory_order_acquire);
    if (primary_) primary_->onCaptureError(error);
    if (secondary && secondary != primary_) secondary->onCaptureError(std::move(error));
}

}  // namespace creator::capture
