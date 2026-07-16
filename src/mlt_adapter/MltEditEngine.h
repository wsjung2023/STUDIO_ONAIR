#pragma once

#include "edit_engine/IEditEngine.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

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
};

class MltEditEngine final : public edit_engine::IEditEngine {
public:
    explicit MltEditEngine(MltEditEngineConfig config);
    ~MltEditEngine() override;

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
    [[nodiscard]] core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::mlt_adapter
