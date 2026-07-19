# ADR-0010: Whisper Transcription Adapter (R2-04 real engine)

## Status
Accepted (gate default OFF; audited native dependency, opt-in build)

## Context
R2-04's gate-free core (`cs_transcription`, ADR-0006) defined
`ITranscriptionProvider` behind a deterministic fake. This lands the REAL local
speech-to-text engine (whisper.cpp) behind that port, using the project's audited
native-dependency recipe (the FFmpeg/MLT template) so the default build and CI stay
green without the heavy dependency, and the engine is verified before it loads.

whisper.cpp and the OpenAI Whisper model weights are MIT (see
`docs/R2-engine-licensing.md`) — no licensing blocker; the gate is engineering.

## Decision
- **Gate**: `option(CS_ENABLE_WHISPER … OFF)`; when ON, requires `CS_WHISPER_ROOT`
  from `scripts/bootstrap_whisper.ps1` (FATAL_ERROR otherwise) — mirrors CS_ENABLE_MLT.
- **Bootstrap** `scripts/bootstrap_whisper.ps1`: pins whisper.cpp **v1.7.6**
  (commit a8d002cf…), downloads + **SHA-256-verifies** the source archive
  (`166140e9…`), CPU-only build/install, downloads + verifies the pinned model
  `ggml-tiny.en.bin` (`921e4cf8…`), emits `CS_WHISPER_ROOT` + a runtime manifest
  (ARCHITECTURE §11).
- **Adapter** `src/whisper_adapter/` (built only when enabled; NOT Qt-free — links
  whisper): `WhisperRuntimeManifest` re-verifies model/artifact SHA + rejects
  GPL/forbidden/traversal before load; `WhisperTranscriptionProvider` implements
  `ITranscriptionProvider` (resamples to 16 kHz mono, runs whisper under a mutex,
  maps segments/tokens → project-timebase `Transcript`).
- **Fallback**: `UnavailableTranscriptionProvider` in `cs_transcription`
  (Qt-free, whisper-free) returns a clear "not built" error; `TranscriptionProviderFactory`
  selects Whisper vs Unavailable by the compile-time gate — mirrors the edit-engine factory.
- **Policy test** `WhisperBootstrapPolicy` (ctest): asserts pin/SHA present, forbidden
  flags absent, gate default-OFF. OSS_BOM rows added for whisper.cpp + the model (MIT).

## Consequences
- Default build (gate OFF): **731/731** green, `cs_assert_qt_free` held —
  `cs_transcription` links no whisper; only the enabled-only adapter does. CI needs
  no whisper download.
- Enabled build (`windows-whisper-*` preset): the adapter + tests compile under /WX
  and a REAL `jfk.wav` inference passes (non-empty transcript, sane monotonic
  in-bounds timestamps) — 10/10 enabled tests.
- Model download hash-verified at bootstrap and re-verified at load; no GPL, CPU-only,
  no non-free flags.
- The transcript editor panel + non-destructive transcript-cut (R2-05) remain editor
  territory (deferred).
