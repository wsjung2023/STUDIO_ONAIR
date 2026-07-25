#pragma once

#include "audio_dsp/ExportLoudnessAnalysis.h"
#include "edit_engine/IEditEngine.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace creator::app {

/// Resolves the current export loudness-normalization setting at render time.
/// Returning a value opts that render into two-pass loudness normalization to
/// the given target; std::nullopt leaves the program untouched. Read per render
/// (not cached) so a user toggling the setting takes effect on the next export.
using ExportLoudnessSettingProvider =
    std::function<std::optional<audio_dsp::ExportLoudnessAnalyzer::Parameters>()>;

/// Export-only composition root. Each render reopens and identity-verifies the
/// project package, recovers interrupted jobs, then creates an independent MLT
/// graph backed by the project's durable render-job store.
class ProjectExportEngine final : public edit_engine::IEditEngine {
public:
    /// `loudnessSettingProvider` is optional; when unset (or when it returns
    /// nullopt) export applies no loudness normalization — the historical
    /// behavior. It is called once per render so the setting is always current.
    explicit ProjectExportEngine(
        std::filesystem::path mltRuntimeRoot,
        ExportLoudnessSettingProvider loudnessSettingProvider = {});

    [[nodiscard]] core::Result<void> load(
        const edit_engine::TimelineSnapshot&) override;
    [[nodiscard]] core::Result<void> update(
        const edit_engine::TimelineChangeSet&) override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> seek(core::TimestampNs) override;
    [[nodiscard]] core::Result<edit_engine::PreviewFrame> requestFrame(
        core::TimestampNs) override;
    [[nodiscard]] core::Result<edit_engine::PreviewAudioBlock> requestMixedAudio(
        core::TimestampNs, std::uint32_t frequency, std::uint32_t channels,
        std::uint32_t samples) override;
    [[nodiscard]] core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override;

private:
    std::filesystem::path mltRuntimeRoot_;
    ExportLoudnessSettingProvider loudnessSettingProvider_;
};

}  // namespace creator::app
