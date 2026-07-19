#include "rnnoise_adapter/RnnoiseDenoiseProcessor.h"

#include "audio_dsp/AudioBuffer.h"
#include "core/AppError.h"
#include "rnnoise_adapter/RnnoiseRuntimeManifest.h"

#include <rnnoise.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace creator::rnnoise_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

// The model is trained on int16-range floats, not the normalized [-1, 1] our
// AudioBuffer carries, so scale into and back out of that domain.
constexpr float kToModelScale = 32768.0F;
constexpr float kFromModelScale = 1.0F / 32768.0F;

/// RAII owner for one rnnoise DenoiseState (CLAUDE.md §4: no raw owning ptr).
struct DenoiseStateDeleter final {
    void operator()(DenoiseState* state) const noexcept {
        if (state != nullptr) rnnoise_destroy(state);
    }
};
using DenoiseStatePtr = std::unique_ptr<DenoiseState, DenoiseStateDeleter>;

/// Independent per-channel state: RNNoise is mono, so each channel gets its own
/// DenoiseState plus the framing accumulator and a bounded output delay ring.
struct ChannelState final {
    DenoiseStatePtr state;
    std::array<float, RnnoiseDenoiseProcessor::kFrameSize> fill{};
    std::size_t fillCount = 0;

    // Output delay ring, bounded at two frames (CLAUDE.md §9: no unbounded
    // queue). Primed with one frame of silence so popping never underflows and
    // the latency is a constant one frame.
    static constexpr std::size_t kRingCapacity =
        2 * RnnoiseDenoiseProcessor::kFrameSize;
    std::array<float, kRingCapacity> ring{};
    std::size_t ringHead = 0;
    std::size_t ringCount = 0;

    void prime() noexcept {
        fillCount = 0;
        ringHead = 0;
        ringCount = RnnoiseDenoiseProcessor::kFrameSize;
        ring.fill(0.0F);
    }

    float pop() noexcept {
        const float value = ring[ringHead];
        ringHead = (ringHead + 1) % kRingCapacity;
        --ringCount;
        return value;
    }

    void push(float value) noexcept {
        const std::size_t tail = (ringHead + ringCount) % kRingCapacity;
        ring[tail] = value;
        ++ringCount;
    }
};

}  // namespace

struct RnnoiseDenoiseProcessor::Impl final {
    std::uint32_t channelCount = 0;
    std::vector<ChannelState> channels;

    // Reallocate one DenoiseState per channel. Returns false if RNNoise could
    // not allocate a state, so process() can surface it rather than crash.
    [[nodiscard]] bool configure(std::uint32_t channels_) {
        channels.clear();
        channels.resize(channels_);
        for (auto& channel : channels) {
            channel.state.reset(rnnoise_create(nullptr));
            if (channel.state == nullptr) {
                channels.clear();
                channelCount = 0;
                return false;
            }
            channel.prime();
        }
        channelCount = channels_;
        return true;
    }
};

RnnoiseDenoiseProcessor::RnnoiseDenoiseProcessor()
    : impl_(std::make_unique<Impl>()) {}
RnnoiseDenoiseProcessor::~RnnoiseDenoiseProcessor() = default;
RnnoiseDenoiseProcessor::RnnoiseDenoiseProcessor(
    RnnoiseDenoiseProcessor&&) noexcept = default;
RnnoiseDenoiseProcessor& RnnoiseDenoiseProcessor::operator=(
    RnnoiseDenoiseProcessor&&) noexcept = default;

std::size_t RnnoiseDenoiseProcessor::latencyFrames() const noexcept {
    return kFrameSize;
}

void RnnoiseDenoiseProcessor::reset() noexcept {
    for (auto& channel : impl_->channels) {
        if (channel.state != nullptr) {
            rnnoise_init(channel.state.get(), nullptr);
        }
        channel.prime();
    }
}

core::Result<void> RnnoiseDenoiseProcessor::process(
    audio_dsp::AudioBuffer& buffer) {
    // An empty buffer is a valid no-op input (IAudioProcessor contract).
    if (buffer.empty()) {
        return core::ok();
    }
    if (buffer.format().sampleRateHz() != kRequiredSampleRateHz) {
        return AppError{ErrorCode::InvalidArgument,
                        "RNNoise denoise requires 48 kHz audio"};
    }

    // Reject non-finite input up front, before mutating anything, so a poisoned
    // sample cannot silently propagate through the model (CLAUDE.md §5/§9).
    for (const float value : buffer.samples()) {
        if (!std::isfinite(value)) {
            return AppError{ErrorCode::InvalidArgument,
                            "RNNoise denoise received a non-finite sample"};
        }
    }

    const std::uint32_t channelCount = buffer.channelCount();
    if (impl_->channelCount != channelCount) {
        if (!impl_->configure(channelCount)) {
            return AppError{ErrorCode::InvalidState,
                            "RNNoise could not allocate denoise state"};
        }
    }

    const std::size_t frameCount = buffer.frameCount();
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
            ChannelState& state = impl_->channels[channel];

            // Emit the delayed output for this position first, then feed the
            // incoming sample. When a frame completes, denoise it and enqueue
            // its 480 outputs behind the one-frame priming delay.
            const float output = state.pop();
            state.fill[state.fillCount++] =
                buffer.sample(frame, channel) * kToModelScale;
            if (state.fillCount == kFrameSize) {
                std::array<float, kFrameSize> denoised{};
                rnnoise_process_frame(state.state.get(), denoised.data(),
                                      state.fill.data());
                for (const float sample : denoised) {
                    state.push(sample * kFromModelScale);
                }
                state.fillCount = 0;
            }
            buffer.sample(frame, channel) = output;
        }
    }
    return core::ok();
}

core::Result<std::unique_ptr<audio_dsp::IAudioProcessor>>
createRnnoiseDenoiseProcessor(const std::filesystem::path& runtimeRoot) {
    if (auto verified = verifyRnnoiseRuntimeManifest(runtimeRoot);
        !verified.hasValue()) {
        return verified.error();
    }
    // Constructed only after the audited prefix verifies (CLAUDE.md §9: no
    // silently-degraded denoiser). Explicit new because the constructor is
    // private to this factory.
    return std::unique_ptr<audio_dsp::IAudioProcessor>{
        new RnnoiseDenoiseProcessor{}};
}

}  // namespace creator::rnnoise_adapter
