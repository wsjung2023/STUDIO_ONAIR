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

## Update (2026-07-24): export two-pass wiring landed
The deferred export wiring is now implemented, user-controlled and off by default:
- `ExportLoudnessAnalyzer::decide(measuredLufs, truePeakDbtp)` was factored out of
  `analyze` so the exporter measures by STREAMING the program through a
  `LoudnessMeter` (no whole-program buffer) and gets the identical decision.
- `MltEditEngine::renderFrozen` runs pass 1 (an audio-only sweep of the mixed 48 kHz
  program — `mlt_frame_get_audio` per frame, no video decode) to measure, then
  rebuilds the graph so `build()` attaches a `GainProcessor(gainDb)` + true-peak
  `LimiterProcessor` (via the existing creator-filter hook, before the sanitize net).
  Gated on `MltEditEngineConfig::exportLoudness` (empty ⇒ untouched, historical
  behavior).
- `ProjectExportEngine` takes an `ExportLoudnessSettingProvider` read once per render;
  `main.cpp` wires it to the `export/loudnessNormalization` QSettings key, and
  `ExportController` exposes a `loudnessNormalization` toggle (ExportPage.qml).
- Verified end-to-end (BS.1770-4): a −27.3 LUFS program exported OFF stays −27.3;
  exported ON lands at −14.13 LUFS with true peak −1.04 dBFS (the −1 dBTP ceiling).
  Unit: `ExportLoudnessAnalysisTest.DecideMatchesAnalyzeForSameMeasurement` +
  `DecideGuardsNoMeasurementAndFloor`.

## Originally deferred / coordinated
- **Export two-pass wiring**: the single insertion in `ProjectExportEngine` /
  `MltEditEngine` render (Pass 1 `ExportLoudnessAnalyzer::analyze` on the mixed 48 kHz
  program; Pass 2 apply `gainDb` via `GainProcessor` + true-peak `LimiterProcessor`
  at the ceiling while writing the consumer). Call site documented in
  `ExportLoudnessAnalysis.h`. Left to the export-file owner to avoid a live collision.
  (Done — see the Update above.)
- **Physical R2-07 gate**: the real 30-minute capture → edit → export → A/V + loudness
  acceptance (enabled presets + real machine) — out of scope for the automated test.
- `cs_cut_suggest` is documented (not asserted) in the acceptance test until it lands
  on this line.

## Consequences
- 16 new audio tests + 3 acceptance tests; full suite **1043/1043** green
  (`/W4 /permissive- /WX`), `cs_assert_qt_free` held. No new dependency; OSS_BOM
  untouched. Only hot-file edit: `main.cpp` (16/4 lines, one spot, inside the
  enabled-MLT block).
