#pragma once

#include "transcription/ITranscriptionProvider.h"

#include <filesystem>
#include <memory>

namespace creator::app {

/// Selection inputs for makeTranscriptionProvider().
struct TranscriptionProviderOptions final {
    /// The audited whisper runtime prefix (CS_WHISPER_ROOT / a staged copy).
    /// Ignored when whisper is not compiled into this build.
    std::filesystem::path whisperRuntimeRoot;

    /// Decoder threads; 0 lets the provider choose. Ignored when unavailable.
    int threadCount{0};
};

/// Returns the transcription provider for this build, mirroring
/// makeLiveRecordingEngine: the audited whisper.cpp provider when
/// CS_APP_ENABLE_WHISPER is defined, otherwise an UnavailableTranscriptionProvider
/// that returns a clear "not built" error. When whisper IS built but its runtime
/// fails preflight, the specific failure is surfaced through an Unavailable
/// provider rather than crashing. Never throws.
[[nodiscard]] std::unique_ptr<transcription::ITranscriptionProvider>
makeTranscriptionProvider(const TranscriptionProviderOptions& options);

}  // namespace creator::app
