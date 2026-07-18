#pragma once

#include "edit_engine/IEditEngine.h"

#include <filesystem>

namespace creator::app {

/// Export-only composition root. Each render reopens and identity-verifies the
/// project package, recovers interrupted jobs, then creates an independent MLT
/// graph backed by the project's durable render-job store.
class ProjectExportEngine final : public edit_engine::IEditEngine {
public:
    explicit ProjectExportEngine(std::filesystem::path mltRuntimeRoot);

    [[nodiscard]] core::Result<void> load(
        const edit_engine::TimelineSnapshot&) override;
    [[nodiscard]] core::Result<void> update(
        const edit_engine::TimelineChangeSet&) override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> seek(core::TimestampNs) override;
    [[nodiscard]] core::Result<edit_engine::PreviewFrame> requestFrame(
        core::TimestampNs) override;
    [[nodiscard]] core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override;

private:
    std::filesystem::path mltRuntimeRoot_;
};

}  // namespace creator::app
