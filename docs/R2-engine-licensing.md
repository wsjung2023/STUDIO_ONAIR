# R2 real-engine model/weight licensing determination

Primary-source checked (upstream LICENSE files + model cards). This is the gate
`legal/LICENSE_POLICY.md` sets for R2-04 (captions) and R2-06 (audio denoise): an
AI model whose **weight** commercial use is unclear cannot ship, regardless of how
permissive the **code** license is. BOM rows are added only when a dep is actually
integrated (CLAUDE §7); this doc is the provenance to use at that point.

| Engine (feature) | Code license | Weight license | Commercial redistribution | Verdict |
|---|---|---|---|---|
| **whisper.cpp** (R2-04 captions) | MIT (ggml-org/whisper.cpp) | **MIT** — OpenAI Whisper weights are MIT (openai/whisper LICENSE); the ggml-converted `.bin` models inherit it | Yes, explicit | **APPROVED** |
| **RNNoise** (R2-06 realtime denoise) | BSD (Xiph.Org / J-M Valin) | included in-repo under the same BSD; models have shipped commercially for years | Yes | **APPROVED** (confirm exact 2- vs 3-clause at integration) |
| **DeepFilterNet** (R2-06 post denoise) | dual **MIT OR Apache-2.0** (Rikorose/DeepFilterNet: LICENSE-MIT + LICENSE-APACHE) | weights inherit the dual license | Yes | **APPROVED** |

## Bottom line
- **Nothing is PROHIBITED.** All three real engines are commercially usable, including
  their weights — unlike R3's OpenSeeFace (which needed a dataset-provenance sign-off),
  these have clean weight licenses.
- The gate that remains is **engineering, not legal**: the audited-native-dependency
  recipe (the MLT/FFmpeg template) — gate option `CS_ENABLE_*` (default OFF), pinned
  version + SHA-256 of source and of each model artifact, bootstrap that rejects
  forbidden features, runtime bidirectional-SHA verification, staging, a policy test,
  C++ manifest verification before load, and an `Unavailable*` fallback when disabled
  (ARCHITECTURE §11 requires model-download hash verification).

## Obligations when each is integrated (add the BOM row then)
- **whisper.cpp**: pin a tag; SHA-256 the chosen ggml model (e.g. `ggml-base`/`small`);
  preserve MIT notices. Slots behind `ITranscriptionProvider` (already built, R2-04 core).
- **RNNoise**: pin a commit; BSD notice; small enough to vendor. Slots into the
  `AudioProcessingChain` ML-denoise slot (index 0, reserved in R2-06 core).
- **DeepFilterNet**: pin a release; choose MIT or Apache-2.0 and record it; SHA the model.
  Heavier (post-process); also the chain denoise slot.

## Ports these land behind (already built, gate-free)
- `ITranscriptionProvider` (R2-04, `cs_transcription`) — whisper.cpp implements it.
- `AudioProcessingChain` reserved ML-denoise slot (R2-06, `cs_audio_dsp`) — RNNoise /
  DeepFilterNet implement `IAudioProcessor` and drop into index 0.

Sources: ggml-org/whisper.cpp LICENSE (MIT); openai/whisper LICENSE (MIT);
xiph/rnnoise (BSD); Rikorose/DeepFilterNet LICENSE-MIT + LICENSE-APACHE (dual).
