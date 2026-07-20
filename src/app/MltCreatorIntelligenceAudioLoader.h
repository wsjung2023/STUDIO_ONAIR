#pragma once

#include "app/CreatorIntelligenceController.h"
#include "mlt_adapter/MltEditEngine.h"

#include <filesystem>

namespace creator::app {

/// Builds the product audio-loader used by local transcription: the current
/// edited MLT mix is cleaned at 48 kHz stereo, then box-filtered to the 16 kHz
/// mono format expected by Whisper and cut analysis.
[[nodiscard]] CreatorIntelligenceController::AudioLoader
makeMltCreatorIntelligenceAudioLoader(
    std::filesystem::path runtimeRoot,
    mlt_adapter::MltEditEngineConfig::AudioProcessorFactory audioFactory);

}  // namespace creator::app
