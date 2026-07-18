# R2-06 Audio Processing Core — parallel track plan

## Context

R1 code is complete (recording, editor, MLT preview, H.264 export, concat segment
fix all committed at `27eb5f1`); only the long physical export gate (R1-07) is
still running. To use that wait productively we open a **parallel-safe R2 slice**.

R2-06 (오디오 정리·음량 표준화) is chosen because its core is a **pure DSP
pipeline behind a port** — no UI, no editor, no ML model — so it isolates exactly
like R3's tracking core and cannot collide with Codex's R2-01 cursor telemetry or
with any R1-07 export fix. It branches from the R1 tip so it sits on the finished
storage/edit/output base per the roadmap.

## Scope — gate-free core only (Stage A)

**In:** loudness measurement + normalization (EBU R128 / ITU-R BS.1770-4),
dynamics (compressor + true-peak limiter), gain staging, a processing chain, a
loudness NDJSON telemetry sink, deterministic synthetic-signal tests.

**Deferred (gated, not now):**
- ML denoise — RNNoise / DeepFilterNet — behind the model-weight **licensing gate**
  (same discipline as R3 Stage B; RNNoise is BSD/clear, DeepFilterNet needs a
  license check before it lands). The chain exposes a slot; no model ships now.
- **Integration into the export/MLT pipeline** (applying the chain during render)
  — that touches Codex's `mlt_adapter`/`ffmpeg_adapter`, so it is post-R1 (Stage C
  equivalent), coordinated after R1-07 closes.

BS.1770/R128 is a published *standard* implemented from spec — no third-party
dependency, no OSS_BOM change.

## New module: `cs_audio_dsp` (Qt-free, `cs_add_qtfree_library`)

DEPENDS: `cs_core` (Result/typed time), `nlohmann_json` (telemetry). No Qt / FFmpeg
/ MLT / capture backend — passes `cs_assert_qt_free` transitive check.

```
src/audio_dsp/
  AudioFormat.h              # sampleRate, channelCount, layout (value object)
  AudioBuffer.h              # non-owning span view over interleaved float PCM + format
  IAudioProcessor.h          # port: process(AudioBuffer) -> Result<void> (in-place chainable node)
  GainProcessor.{h,cpp}      # linear gain in dB, click-free ramp
  KWeightingFilter.{h,cpp}   # BS.1770 stage 1 (shelving) + stage 2 (high-pass) biquads
  LoudnessMeter.{h,cpp}      # BS.1770-4: K-weight -> mean-square -> gated integrated LUFS; momentary/short-term; true-peak (4x oversample)
  DynamicsProcessor.{h,cpp}  # compressor (threshold/ratio/knee/attack/release) + brickwall look-ahead true-peak limiter
  LoudnessNormalizer.{h,cpp} # measure integrated LUFS -> apply single gain to hit target (e.g. -14 LUFS), then limiter guards true peak
  AudioProcessingChain.{h,cpp} # ordered IAudioProcessor list; deferred ML-denoise slot documented
  AudioLoudnessSample.h      # measurement value object (integrated/short/momentary LUFS, true-peak dBTP, tNs)
  AudioLoudnessSerializer.{h,cpp}  # -> audio.loudness JSON (event.schema.json if present; else own schema)
  AudioLoudnessNdjsonSink.{h,cpp}  # telemetry/audio-loudness.ndjson atomic append
src/fakes/  (or tests/audio_dsp/support/)
  SyntheticAudio.{h,cpp}     # deterministic sine / pink / white / silence / burst generators (no clock)
tests/audio_dsp/
  KWeightingFilterTest / LoudnessMeterTest    # anchored to BS.1770 reference values
  DynamicsProcessorTest / GainProcessorTest
  LoudnessNormalizerTest / AudioProcessingChainTest
  AudioLoudnessSerializerTest / AudioLoudnessNdjsonSinkTest
```

## Correctness anchors (this is a numeric module — pin ground truth)

- **BS.1770 reference:** a 1 kHz sine at −20 dBFS (0 dBFS = full scale) integrated
  loudness must read **−20.0 LUFS ±0.1** through K-weighting (the standard's own
  calibration point). Stereo −20 dBFS sine → ~−17.0 LUFS (channel summation). Anchor
  the filter coefficients to the BS.1770-4 tables at 48 kHz and assert step/impulse
  response, not just self-consistency.
- **Gating:** absolute −70 LUFS gate + relative −10 LU gate (integrated only).
- **True peak:** 4× oversampled, must catch inter-sample peaks a sample-peak meter
  misses; assert on a signal whose true peak exceeds its sample peak.
- **Normalizer:** after normalize-to-target, re-measured integrated LUFS ≈ target
  ±0.5; limiter guarantees true peak ≤ ceiling (e.g. −1 dBTP) with no overshoot.
- **Determinism:** no clock reads, no sleeps, no RNG (synthetic signals are formula-
  driven). Assert exact/near-exact values, exact event counts.

## Verification (CLAUDE §8)

unit + error-path (invalid format, empty buffer, NaN/Inf sample rejection) +
log/metric (loudness telemetry asserted) + resource cleanup (sink fixture teardown)
+ docs. Build+full ctest green via `scripts/studio-build-verify.ps1`; `cs_audio_dsp`
passes `cs_assert_qt_free`.

## Parallel-development contract (files shared with R1/Codex)

Only: root `CMakeLists.txt` (`add_subdirectory(src/audio_dsp)` in the Qt-free block),
`src/fakes/CMakeLists.txt` (+ synthetic-audio source, if placed there),
`tests/CMakeLists.txt` (audio_dsp test sources). Everything else net-new. Does not
touch `mlt_adapter`, `ffmpeg_adapter`, `app`, editor, timeline, cursor (R2-01), or
any DB migration → no collision with R1-07 or R2-01.
