#pragma once

#include "core/Result.h"
#include "transcription/ITranscriptionProvider.h"

namespace creator::transcription {

/// A deterministic ITranscriptionProvider that needs no model, clock, or RNG.
///
/// Responsibility: prove the transcription pipeline (port -> domain ->
/// serialization -> storage) end to end before the real whisper.cpp engine
/// exists, exactly as FakeCaptureSource proves the capture port before the OS
/// backends land. It scripts a Transcript purely from the input's DURATION and
/// format: a fixed vocabulary is spaced across the audio at a fixed cadence and
/// grouped into fixed-size segments. It never reads sample VALUES to decide the
/// content (so it can never depend on a wall clock or randomness), but it does
/// enforce the port's precondition contract (non-empty, all-finite audio).
///
/// The same AudioInput and options always yield an identical Transcript, which
/// is what makes the pipeline testable with exact assertions.
class FakeTranscriptionProvider final : public ITranscriptionProvider {
public:
    FakeTranscriptionProvider() = default;

    /// Emits the scripted Transcript. Fails per the ITranscriptionProvider
    /// contract: InvalidArgument for zero-frame or non-finite audio.
    [[nodiscard]] core::Result<Transcript> transcribe(
        const AudioInput& audio, const TranscriptionOptions& options) override;

    /// Cadence and grouping are part of the public, asserted contract of this
    /// fake, so tests can compute the expected word/segment layout.
    static constexpr core::DurationNs kWordSlot{core::Nanoseconds{500'000'000}};   // 500 ms
    static constexpr core::DurationNs kWordSpoken{core::Nanoseconds{400'000'000}};  // 400 ms
    static constexpr int kWordsPerSegment = 5;
};

}  // namespace creator::transcription
