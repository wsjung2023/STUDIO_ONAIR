# ADR-0011: RNNoise Denoise Adapter (R2-06 real engine)

## Status
Accepted (gate default OFF; audited native dependency, opt-in build)

## Context
R2-06's gate-free audio core (`cs_audio_dsp`, ADR-0005) reserved an ML-denoise slot
in `AudioProcessingChain`. This lands the real RNNoise denoiser behind that slot as
an `IAudioProcessor`, via the audited native-dependency recipe (FFmpeg/MLT/whisper
template), so the default build/CI stays green without the dependency and the
library is verified before load. RNNoise is BSD-3-Clause; the model weights are
compiled into the source, so pinning+hashing the single source archive fixes both
code and weights (no separate model artifact).

## Decision
- **Gate**: `option(CS_ENABLE_RNNOISE … OFF)`; when ON requires `CS_RNNOISE_ROOT`
  from `scripts/bootstrap_rnnoise.ps1` (FATAL_ERROR otherwise) — mirrors CS_ENABLE_MLT.
- **Bootstrap** `scripts/bootstrap_rnnoise.ps1`: pins rnnoise **v0.1.1** (commit
  6cbfd53e…), downloads + **SHA-256-verifies** the source archive (`1641712c…`),
  CPU-only static build, installs lib+header, emits `CS_RNNOISE_ROOT` + a runtime
  manifest. `scripts/verify_rnnoise_runtime.ps1` is a fail-closed prefix verifier
  (identity, per-file SHA, forbidden/GPL/traversal/reparse) run at configure time.
- **Adapter** `src/rnnoise_adapter/` (enabled-only; links rnnoise; Qt-free like
  cs_mlt_adapter): `RnnoiseRuntimeManifest` verifies before load;
  `RnnoiseDenoiseProcessor` implements `IAudioProcessor` — frames to 480-sample /
  48 kHz, one independent `DenoiseState` per channel, ×32768 int16 domain scaling,
  fixed one-frame (480) latency via a primed delay ring reported by `latencyFrames()`,
  non-48 kHz / non-finite rejected, `reset()` re-primes; bounded, RAII, no exceptions
  across the boundary.
- **Fallback**: `UnavailableDenoiseProcessor` (in `cs_audio_dsp`, Qt-free +
  rnnoise-free) returns a clear "not built" error; the assembly site selects real vs
  Unavailable by the compile-time gate (mirrors MltEditEngine vs UnavailableEditEngine).
- **Policy test** `RnnoiseBootstrapPolicy` (ctest); OSS_BOM row (BSD-3-Clause, pin+SHA,
  in-source weights).

## Consequences
- Default build (OFF): **766/766** green, `cs_assert_qt_free` held — `cs_audio_dsp`
  links no rnnoise; the adapter subdir isn't added when OFF. CI needs no rnnoise.
- **Known limitation (documented, not hidden — CLAUDE §9):** rnnoise v0.1.1's
  CELT-derived sources use C99 variable-length arrays, which MSVC `cl.exe` does not
  support; the upstream static lib therefore does **not** build on pure-MSVC. The
  bootstrap auto-selects `clang-cl`/LLVM (ABI-compatible) when present, so the enabled
  build works on any machine with gcc/clang or the LLVM toolset; on pure-MSVC it fails
  with a clear VLA error. The adapter translation units themselves compile cleanly
  under MSVC /WX against `rnnoise.h`.
- No GPL; CPU-only; BSD-3-Clause notice preserved.
- Applying the denoise node during editor/export render is the integration step
  (Codex, on the R1 editor/mlt), consuming this `IAudioProcessor`.
