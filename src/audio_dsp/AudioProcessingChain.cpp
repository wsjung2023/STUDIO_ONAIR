#include "audio_dsp/AudioProcessingChain.h"

namespace creator::audio_dsp {

AudioProcessingChain& AudioProcessingChain::add(
    std::unique_ptr<IAudioProcessor> node) {
    // A null node (e.g. from a create() that failed upstream) is dropped rather
    // than stored, so process() never has to guard against a null entry.
    if (node != nullptr) {
        nodes_.push_back(std::move(node));
    }
    return *this;
}

core::Result<void> AudioProcessingChain::process(AudioBuffer& buffer) {
    // Run nodes in order over the same in-place buffer. Stop at the first
    // failure and return it — a later node never sees a buffer an earlier node
    // could not produce (CLAUDE.md §5).
    for (const std::unique_ptr<IAudioProcessor>& node : nodes_) {
        core::Result<void> r = node->process(buffer);
        if (!r.hasValue()) {
            return r;
        }
    }
    return core::ok();
}

std::size_t AudioProcessingChain::latencyFrames() const noexcept {
    std::size_t total = 0;
    for (const std::unique_ptr<IAudioProcessor>& node : nodes_) {
        total += node->latencyFrames();
    }
    return total;
}

}  // namespace creator::audio_dsp
