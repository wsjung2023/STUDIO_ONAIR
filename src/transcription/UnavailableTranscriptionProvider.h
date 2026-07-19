#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "transcription/ITranscriptionProvider.h"

namespace creator::transcription {

/// The ITranscriptionProvider used when the real whisper.cpp engine is not built
/// into this configuration (CS_ENABLE_WHISPER=OFF).
///
/// Responsibility: fail loudly and clearly instead of pretending to transcribe.
/// It is the transcription mirror of edit_engine::UnavailableEditEngine: it keeps
/// cs_transcription Qt-free AND whisper-free while still letting the whole
/// pipeline compile, link, and be exercised in the default build. Every call
/// returns the same documented AppError so callers surface "captions are not
/// available in this build" rather than silently dropping the request
/// (CLAUDE.md 9).
///
/// It can also carry a specific error: when whisper IS built but its runtime
/// fails preflight (missing/altered model, unloadable library), the factory
/// wraps that precise failure in one of these so the reason still reaches the UI.
class UnavailableTranscriptionProvider final : public ITranscriptionProvider {
public:
    /// The default "not built in this configuration" error.
    UnavailableTranscriptionProvider();

    /// Surfaces a specific precondition failure (e.g. a whisper preflight error).
    explicit UnavailableTranscriptionProvider(core::AppError error);

    [[nodiscard]] core::Result<Transcript> transcribe(
        const AudioInput& audio, const TranscriptionOptions& options) override;

    /// The error this provider will return; exposed so callers can log the
    /// reason without invoking transcribe().
    [[nodiscard]] const core::AppError& reason() const noexcept { return error_; }

private:
    core::AppError error_;
};

}  // namespace creator::transcription
