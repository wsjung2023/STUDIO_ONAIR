#pragma once

#include "core/Result.h"
#include "transcription/ITranscriptionProvider.h"

#include <filesystem>
#include <memory>

namespace creator::whisper_adapter {

/// Construction parameters for the whisper.cpp-backed provider.
struct WhisperProviderConfig final {
    /// The audited whisper.cpp install prefix (CS_WHISPER_ROOT). Its
    /// whisper-runtime-manifest.json is verified before the model is loaded.
    std::filesystem::path runtimeRoot;

    /// Decoder threads. 0 asks the provider to pick a sensible default from the
    /// hardware concurrency; never reads a wall clock.
    int threadCount{0};
};

/// The real ITranscriptionProvider: OpenAI Whisper inference via whisper.cpp.
///
/// Responsibility: turn decoded project-timebase PCM into a Transcript using the
/// audited, hash-verified ggml model, translating every whisper/ggml failure into
/// core::Result / core::AppError. It is an ADAPTER (like mlt_adapter): it links
/// whisper and lives outside the Qt-free/whisper-free cs_transcription core, but
/// it still never lets an exception cross the port boundary (CLAUDE.md 4/5).
///
/// Audio contract: whisper requires 16 kHz mono float PCM in [-1, 1]. This
/// provider resamples the caller's AudioInput (any rate, any channel count) to
/// that format with an interleaved-average downmix and linear resampling; the
/// mapping onto the project timebase is derived from the ORIGINAL audio duration
/// (CLAUDE.md 2.3), so resampling never shifts a timestamp.
///
/// Threading: whisper_context is not thread-safe, so transcribe() is serialized
/// on an internal mutex. A caller wanting concurrency creates one provider per
/// worker. transcribe() is blocking; run it off the UI thread (CLAUDE.md 9).
class WhisperTranscriptionProvider final
    : public transcription::ITranscriptionProvider {
public:
    /// Verifies the runtime manifest and loads the model. Fails (never throws)
    /// with FailedPrecondition-style errors if the prefix is not the audited one
    /// or the model cannot be initialized.
    [[nodiscard]] static core::Result<std::unique_ptr<WhisperTranscriptionProvider>>
    create(WhisperProviderConfig config);

    ~WhisperTranscriptionProvider() override;

    WhisperTranscriptionProvider(const WhisperTranscriptionProvider&) = delete;
    WhisperTranscriptionProvider& operator=(const WhisperTranscriptionProvider&) =
        delete;

    /// Runs inference and maps whisper segments/tokens to a Transcript. Enforces
    /// the ITranscriptionProvider precondition contract (InvalidArgument for
    /// zero-frame or non-finite audio) exactly like the fake.
    [[nodiscard]] core::Result<transcription::Transcript> transcribe(
        const transcription::AudioInput& audio,
        const transcription::TranscriptionOptions& options) override;

private:
    class Impl;
    explicit WhisperTranscriptionProvider(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::whisper_adapter
