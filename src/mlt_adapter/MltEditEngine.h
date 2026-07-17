#pragma once

#include "edit_engine/IEditEngine.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace creator::mlt_adapter {

struct MltEditEngineConfig final {
    std::filesystem::path runtimeRoot;
    std::uint32_t previewWidth{1280};
    std::uint32_t previewHeight{720};
};

struct MltEditEngineDiagnostics final {
    std::size_t nativeTrackCount{};
    std::size_t videoCompositeTransitions{};
    std::size_t audioMixTransitions{};
    std::size_t visualBranchCount{};
    std::size_t transformedVisualBranchCount{};
    std::size_t audioEnvelopeBranchCount{};
    std::size_t missingOverlayCount{};
};

class MltEditEngine final : public edit_engine::IEditEngine {
public:
    explicit MltEditEngine(MltEditEngineConfig config);
    ~MltEditEngine() override;

    /// Verifies both the signed file set and the Windows loader boundary before
    /// any delay-imported MLT symbol is called by the application.
    [[nodiscard]] static core::Result<void> preflightRuntime(
        const std::filesystem::path& runtimeRoot);

    /// Verifies and binds the audited process-global MLT runtime. Repeated
    /// calls for the same canonical runtime are safe.
    [[nodiscard]] static core::Result<void> initializeRuntime(
        const std::filesystem::path& runtimeRoot);

    [[nodiscard]] core::Result<void> load(
        const edit_engine::TimelineSnapshot& snapshot) override;
    [[nodiscard]] core::Result<void> update(
        const edit_engine::TimelineChangeSet& change) override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> seek(core::TimestampNs position) override;
    [[nodiscard]] core::Result<edit_engine::PreviewFrame> requestFrame(
        core::TimestampNs position) override;
    [[nodiscard]] core::Result<MltEditEngineDiagnostics> diagnostics() const;
    [[nodiscard]] core::Result<std::vector<float>> requestMixedAudio(
        core::TimestampNs position, int frequency, int channels, int samples);
    [[nodiscard]] core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override;

private:
    [[nodiscard]] core::Result<void> renderFrozen(
        const edit_engine::RenderRequest& request, std::stop_token stopToken,
        const std::function<bool(edit_engine::RenderJobState, double,
                                 core::TimestampNs)>& report);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::mlt_adapter
