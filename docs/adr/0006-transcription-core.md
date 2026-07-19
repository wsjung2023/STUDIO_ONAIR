# ADR-0006: Transcription / Captions Core (R2-04)

## Status
Accepted (gate-free core; real whisper.cpp engine and editor panel deferred)

## Context
R2-04 ("whisper.cpp 로컬 자막·단어/문장 타임스탬프") needs local speech-to-text with
word/segment timestamps in the project timebase. Built as a parallel-safe slice
(no editor, no model) on the R1 tip, mirroring the R3 tracking-core and R2-06
audio-core approach: prove the pipeline behind a port with a deterministic fake so
the real ML engine can land later without reworking the boundary.

## Decision
Add a Qt-free leaf module `cs_transcription` (`src/transcription/`, DEPENDS
`cs_core`, `cs_domain`, `nlohmann_json`):

- `TranscriptWord` / `TranscriptSegment` / `Transcript` — validated value objects;
  timestamps are `domain::TimeRange` in the **project timebase** (CLAUDE §2.3);
  segments and words are validated ordered, non-overlapping, in-range; text is
  validated UTF-8; confidence ∈ [0,1].
- `AudioInput` — a minimal non-owning PCM view (span + rate + channels) local to
  this module, so transcription does NOT couple to `cs_audio_dsp`.
- `ITranscriptionProvider` — the port (`transcribe(AudioInput, options) ->
  Result<Transcript>`), no exceptions across the boundary.
- `FakeTranscriptionProvider` — deterministic scripted transcript from input
  duration alone (no clock, no RNG, no model), the pipeline's test driver.
- `TranscriptSerializer` (+ `schemas/transcript.schema.json`, schemaVersion 1) —
  round-trip JSON; deserialization re-validates through the domain `create()`.
- `TranscriptStore` — durable transcript artifact (temp + flush + atomic rename,
  path-traversal-guarded) written to the package, never the project DB (§6).

## Deferred (explicit, gated)
- **Real whisper.cpp provider** — behind the audited-native-dependency recipe plus
  the model-weight note: whisper.cpp is MIT; the ggml model weights need an
  OSS_BOM entry / license note before shipping. No dependency added now.
- **Transcript editor panel and non-destructive transcript-cut editing (R2-05)** —
  live in the editor/app (Codex territory), sequenced with R1/R2 editor work.

## Consequences
- 48 module tests / 725 total green through `scripts/studio-build-verify.ps1`
  (`/W4 /permissive- /WX`), `cs_assert_qt_free` held (module links no Qt).
- Local durable-write helper duplicated intentionally to keep the narrow dependency
  set (no SQLite/edit-engine layer pulled into transcription).
- Schema conformance is guaranteed by construction; the JSON validator is a
  test-only dependency.
- No new third-party dependency; `legal/OSS_BOM.csv` unchanged.
