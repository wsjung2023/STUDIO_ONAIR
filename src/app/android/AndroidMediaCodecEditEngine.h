#pragma once

#include "edit_engine/IEditEngine.h"

namespace creator::app::android {

/// Android export-only engine. Java owns MediaCodec/MediaMuxer work; this
/// adapter owns immutable request validation, durable job state, cancellation,
/// and atomic publication.
class AndroidMediaCodecEditEngine final : public edit_engine::IEditEngine {
public:
    [[nodiscard]] core::Result<void> load(
        const edit_engine::TimelineSnapshot& snapshot) override;
    [[nodiscard]] core::Result<void> update(
        const edit_engine::TimelineChangeSet& change) override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> seek(core::TimestampNs position) override;
    [[nodiscard]] core::Result<edit_engine::PreviewFrame> requestFrame(
        core::TimestampNs position) override;
    [[nodiscard]] core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override;
};

}  // namespace creator::app::android
