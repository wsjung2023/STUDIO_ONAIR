# ADR-0013: Export Loudness Normalization, Audio Cleanup Chain, R2 Acceptance (R2-06/R2-07)

## Status
Accepted (isolated logic + automated acceptance; the export two-pass wiring and the
physical R2-07 gate are coordinated/deferred)

## Context
On `main`, the R2 audio-to-export integration already applies an RNNoise denoise
node to the preview+export audio chain (via `main.cpp` → `MltEditEngineConfig.
audioProcessingChain`). Two R2-06/R2-07 gaps remained: the chain carried only
denoise (no dynamics), loudness normalization ("음량 표준화") was applied nowhere,
and there was no automated R2 acceptance test. This was built collision-safe while
another agent actively edits the export/compositor code — no edits to
`MltEditEngine.cpp` / `ProjectExportEngine.*`.

## Decision
- `AudioCleanupChain` (`src/audio_dsp/`) — `makeAudioCleanupChain(...)` builds the
  standard cleanup `AudioProcessingChain` (optional denoise → `CompressorProcessor`
  → true-peak `LimiterProcessor`, sane defaults). `main.cpp` now builds the chain
  through it (was denoise-only) — a single minimal edit in the existing
  `CS_APP_ENABLE_RNNOISE` block.
- `ExportLoudnessAnalysis` (`src/audio_dsp/`) — `ExportLoudnessAnalyzer` performs the
  offline two-pass loudness DECISION over a whole mixed-program PCM buffer (measures
  integrated LUFS via `LoudnessMeter`, returns the gain to hit target and whether to
  normalize), lock-step with `LoudnessNormalizer`'s applied gain. It is the reusable
  measure-and-decide half of export loudness normalization; it is intentionally NOT
  yet wired into the render path.
- `R2CreatorIntelligenceAcceptanceTest` (`tests/acceptance/`) — a default-build
  (gate-OFF, fakes) end-to-end check of the R2 intelligence surface: transcription
  round-trip + schema, cursor → auto-zoom + emphasis, cleanup-chain + loudness-to-
  target. Cross-cutting invariants (project timebase, loudness reaches target,
  schema-valid artifacts) asserted.

## Deferred / coordinated
- **Export two-pass wiring**: the single insertion in `ProjectExportEngine` /
  `MltEditEngine` render (Pass 1 `ExportLoudnessAnalyzer::analyze` on the mixed 48 kHz
  program; Pass 2 apply `gainDb` via `GainProcessor` + true-peak `LimiterProcessor`
  at the ceiling while writing the consumer). Call site documented in
  `ExportLoudnessAnalysis.h`. Left to the export-file owner to avoid a live collision.
- **Physical R2-07 gate**: the real 30-minute capture → edit → export → A/V + loudness
  acceptance (enabled presets + real machine) — out of scope for the automated test.
- `cs_cut_suggest` is documented (not asserted) in the acceptance test until it lands
  on this line.

## Consequences
- 16 new audio tests + 3 acceptance tests; full suite **1043/1043** green
  (`/W4 /permissive- /WX`), `cs_assert_qt_free` held. No new dependency; OSS_BOM
  untouched. Only hot-file edit: `main.cpp` (16/4 lines, one spot, inside the
  enabled-MLT block).
