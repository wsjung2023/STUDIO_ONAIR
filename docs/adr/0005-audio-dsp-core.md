# ADR-0005: Audio Processing Core (R2-06)

## Status
Accepted (gate-free core; ML denoise and export integration deferred)

## Context
R2-06 ("오디오 정리·음량 표준화") needs loudness standardization and dynamics for
recorded programs. R1 (recording, editor, MLT preview, H.264 export) is complete,
so this was built as a parallel-safe slice on the R1 tip while R1-07 physical
verification ran — chosen precisely because its core is a pure DSP pipeline behind
a port, with no UI, editor, ML model, or capture-backend coupling.

## Decision
Add a new Qt-free leaf module `cs_audio_dsp` (`src/audio_dsp/`, DEPENDS `cs_core`,
`nlohmann_json`) exposing a small in-place processor port and the DSP nodes needed
for standardization:

- `IAudioProcessor` — in-place, `Result`-based, chainable port with a
  `latencyFrames()` hook for A/V-sync compensation.
- `AudioFormat` / `AudioBuffer` — validated interleaved-float32 value object and a
  non-owning view (RAII: storage lives in the caller).
- `GainProcessor`, `CompressorProcessor` (soft-knee, linked), `LimiterProcessor`
  (look-ahead true-peak, reports latency), `AudioProcessingChain` (composite,
  sums latency, documents a deferred ML-denoise slot at index 0).
- `KWeightingFilter` + `LoudnessMeter` — ITU-R BS.1770-4 / EBU R128 momentary,
  short-term, gated-integrated LUFS and ≥4× true-peak.
- `LoudnessNormalizer` — offline two-pass measure→gain→limit to a target LUFS.
- `AudioLoudnessSample` / `Serializer` / `NdjsonSink` — schema-validated
  `audio.loudness` telemetry as NDJSON to the package (never the project DB).

Loudness math is implemented from the published standards (no third-party
dependency, OSS BOM unchanged). Correctness is anchored to the standard, not to
self-consistency: the EBU Tech 3341 calibration (stereo 1 kHz −23 dBFS →
−23.0 LUFS) is a unit test and reads −22.99 LUFS; normalization to −14 LUFS lands
−14.000.

## Deferred (explicit, gated)
- **ML denoise (RNNoise / DeepFilterNet)** — behind the model-weight licensing gate
  (LICENSE_POLICY / OSS BOM), same discipline as the R3 tracking engine. The chain
  reserves the slot; no model ships now. RNNoise is BSD (clear); DeepFilterNet needs
  a weight-license determination before landing.
- **Export/MLT integration** — applying the chain and writing the normalized audio
  during render touches `mlt_adapter`/`ffmpeg_adapter` (Codex/R1 territory), so it is
  sequenced after R1-07 closes, using the module's port and reported latency.

## Consequences / known limits (honest bounds — CLAUDE §9)
- **48 kHz only** at present: K-weighting coefficients are the 48 kHz tables;
  other rates return `InvalidArgument` (documented TODO to recompute or resample).
- **True peak is a 4× estimate**: `LimiterProcessor` limits the 4×-oversampled
  true-peak estimate, which can under-read the real inter-sample peak by ~0.7 dB
  near Nyquist. The class doc states this and a configurable
  `oversampleSafetyMarginDb` is provided for strict dBTP compliance.
- **Denormal flush**: IIR feedback state (biquads, envelope, limiter release) is
  flushed to zero below 1e-20 to avoid x86 subnormal CPU stalls / real-time drops;
  this is a local flush, not a global MXCSR change.
- **Channel order** assumed L,R,C,Ls,Rs with weights {1,1,1,1.41,1.41}, bounds-safe
  for mono and >5 channels; an explicit-layout TODO is documented.
- Telemetry sink flushes each line to the OS (ordered, append, no clobber) but is
  not fsync-durable against power loss (documented).

## Verification
698→761 tests green through the build-verify wrapper (`scripts/studio-build-verify.ps1`,
`/W4 /permissive- /WX`), `cs_assert_qt_free` held (module links no Qt). Per-piece TDD
plus an independent adversarial whole-module review (no blockers; the two majors —
denormal flush and the overstated true-peak guarantee — were fixed before this ADR).
