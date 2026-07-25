#pragma once

#include "edit_engine/IEditEngine.h"

#include <cstdint>

namespace creator::edit_engine {

class UnavailableEditEngine final : public IEditEngine {
public:
    [[nodiscard]] core::Result<void> load(
        const TimelineSnapshot& snapshot) override;
    [[nodiscard]] core::Result<void> update(
        const TimelineChangeSet& change) override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> seek(core::TimestampNs position) override;
    [[nodiscard]] core::Result<PreviewFrame> requestFrame(
        core::TimestampNs position) override;
    [[nodiscard]] core::Result<PreviewAudioBlock> requestMixedAudio(
        core::TimestampNs position, std::uint32_t frequency,
        std::uint32_t channels, std::uint32_t samples) override;
    [[nodiscard]] core::Result<std::unique_ptr<IRenderJob>> render(
        const RenderRequest& request) override;
};

}  // namespace creator::edit_engine
