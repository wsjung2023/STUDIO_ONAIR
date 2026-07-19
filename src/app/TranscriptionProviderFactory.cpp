#include "app/TranscriptionProviderFactory.h"

#include "transcription/UnavailableTranscriptionProvider.h"

#if defined(CS_APP_ENABLE_WHISPER)
#include "whisper_adapter/WhisperTranscriptionProvider.h"
#endif

#include <utility>

namespace creator::app {

std::unique_ptr<transcription::ITranscriptionProvider> makeTranscriptionProvider(
    const TranscriptionProviderOptions& options) {
#if defined(CS_APP_ENABLE_WHISPER)
    whisper_adapter::WhisperProviderConfig config;
    config.runtimeRoot = options.whisperRuntimeRoot;
    config.threadCount = options.threadCount;
    auto provider = whisper_adapter::WhisperTranscriptionProvider::create(
        std::move(config));
    if (!provider.hasValue()) {
        // Whisper is built but its audited runtime failed preflight: surface the
        // exact reason instead of a generic "not built" message.
        return std::make_unique<transcription::UnavailableTranscriptionProvider>(
            provider.error());
    }
    return std::move(provider).value();
#else
    static_cast<void>(options);
    return std::make_unique<transcription::UnavailableTranscriptionProvider>();
#endif
}

}  // namespace creator::app
