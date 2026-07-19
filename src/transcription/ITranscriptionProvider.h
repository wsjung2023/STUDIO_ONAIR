#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "transcription/AudioInput.h"
#include "transcription/Transcript.h"

#include <string>

namespace creator::transcription {

/// Caller-supplied request parameters for a transcription.
///
/// Responsibility: say which project source the audio belongs to (so the
/// resulting Transcript can be attributed and persisted per CLAUDE.md 2.6/6) and
/// hint the target language. The audio samples themselves live in AudioInput;
/// this struct carries only the metadata the port needs, none of it from Qt,
/// FFmpeg, or MLT.
struct TranscriptionOptions final {
    /// The project source the audio was captured from. Stamped onto the
    /// resulting Transcript.
    domain::SourceId sourceId;

    /// Requested language as a BCP-47-shaped tag (e.g. "en", "ko", "en-US"). May
    /// be empty, in which case the provider chooses a default. A real engine
    /// treats this as a hint / forced language; the fake echoes it.
    std::string languageTag;
};

/// Port for turning decoded audio into a Transcript.
///
/// Responsibility: define the single boundary between the application and any
/// speech-to-text engine. Implementations translate their engine's failures
/// into core::Result / core::AppError and MUST NOT let an exception cross this
/// boundary (CLAUDE.md 4). The domain layer above stays free of any engine type
/// (CLAUDE.md 3/5).
///
/// The real implementation is whisper_adapter::WhisperTranscriptionProvider, an
/// ADAPTER (like mlt_adapter) that links whisper.cpp and is built only behind the
/// CS_ENABLE_WHISPER native-dependency + model-weight gate (whisper.cpp and the
/// ggml Whisper weights are both MIT; see legal/OSS_BOM.csv and
/// docs/R2-engine-licensing.md). This core stays Qt-free AND whisper-free: when
/// the gate is OFF, app::makeTranscriptionProvider hands back an
/// UnavailableTranscriptionProvider that returns a clear error (CLAUDE.md 9).
/// FakeTranscriptionProvider still drives this port deterministically so the
/// domain, serialization, and storage below it are provable without a model.
class ITranscriptionProvider {
public:
    virtual ~ITranscriptionProvider() = default;

    /// Transcribes `audio` under `options`, returning the Transcript on success
    /// or an AppError describing the failure. Never throws across this boundary.
    ///
    /// Contract shared by every implementation:
    ///  - audio with zero frames (empty / zero duration) is InvalidArgument:
    ///    there is nothing to transcribe, and a silent empty Transcript would
    ///    hide the fact that no audio arrived (CLAUDE.md 5, 9);
    ///  - audio containing a non-finite sample (NaN/inf) is InvalidArgument: a
    ///    corrupt buffer is surfaced, not silently transcribed;
    ///  - options.sourceId identifies the resulting Transcript's source.
    [[nodiscard]] virtual core::Result<Transcript> transcribe(
        const AudioInput& audio, const TranscriptionOptions& options) = 0;

protected:
    ITranscriptionProvider() = default;
    ITranscriptionProvider(const ITranscriptionProvider&) = default;
    ITranscriptionProvider(ITranscriptionProvider&&) = default;
    ITranscriptionProvider& operator=(const ITranscriptionProvider&) = default;
    ITranscriptionProvider& operator=(ITranscriptionProvider&&) = default;
};

}  // namespace creator::transcription
