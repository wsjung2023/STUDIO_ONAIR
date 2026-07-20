#pragma once

#include "audio_dsp/ExportLoudnessAnalysis.h"
#include "audio_dsp/IAudioProcessor.h"
#include "edit_engine/IEditEngine.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace creator::app {

/// Export-only composition root. Each render reopens and identity-verifies the
/// project package, recovers interrupted jobs, then creates an independent MLT
/// graph backed by the project's durable render-job store.
class ProjectExportEngine final : public edit_engine::IEditEngine {
public:
    using AudioProcessorFactory = std::function<
        core::Result<std::unique_ptr<audio_dsp::IAudioProcessor>>() >;

    explicit ProjectExportEngine(
        std::filesystem::path mltRuntimeRoot,
        AudioProcessorFactory audioProcessingFactory = {},
        audio_dsp::ExportLoudnessAnalyzer::Parameters loudness = {});

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
    AudioProcessorFactory audioProcessingFactory_;
    audio_dsp::ExportLoudnessAnalyzer::Parameters loudness_;
};

}  // namespace creator::app
