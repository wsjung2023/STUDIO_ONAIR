# ADR-0012: Cut-Suggestion Core (R2-05)

## Status
Accepted (gate-free detection core; non-destructive cut edit deferred)

## Context
R2-05 ("transcript 비파괴 컷·침묵/필러 제안") proposes spans a creator can remove:
silence (dead audio) and filler words. The DETECTION is a pure deterministic
algorithm over the recorded audio + its transcript; the actual non-destructive cut
is an editor/edit-engine concern (deferred). Built on the R1 line, which already
carries both `cs_audio_dsp` and `cs_transcription`.

## Decision
New Qt-free module `cs_cut_suggest` (`src/cut_suggest/`, DEPENDS `cs_audio_dsp`,
`cs_transcription`, `cs_core`, `cs_domain`, `nlohmann_json`):

- `CutSuggestion` — validated: `domain::TimeRange` (project timebase) + `CutReason`
  {Silence, Filler} + score ∈ [0,1] + optional label.
- `CutSuggestParameters` — validated tunables (−45 dBFS, 500 ms min-silence, 20 ms
  RMS window) + a normalized, configurable filler lexicon (default
  um/uh/er/ah/hmm/like/you know/i mean; case- and punctuation-insensitive).
- `SilenceDetector` — RMS-window scan over `audio_dsp::AudioBuffer`; a run below
  threshold ≥ min-silence → a `Silence` suggestion (score = length + depth).
- `FillerDetector` — longest-lexicon-match over normalized transcript words
  (multi-word "you know" spans both), score = mean covered-word confidence;
  rejects unsorted word input.
- `CutSuggestionAnalyzer` — runs both, returns a globally time-ordered union
  (ties Filler-before-Silence; same-reason never overlaps; cross-reason overlaps
  preserved as independent suggestions — dedup deferred to apply).
- `CutSuggestionSerializer` + `schemas/cut_suggestion.schema.json` (v1).

## Deferred (explicit)
- The actual non-destructive CUT / timeline edit applying these suggestions,
  ripple/gap handling, and the review UI (editor + edit-engine, Codex R1 territory).

## Consequences
- 38 module tests / 1015 total green via `scripts/studio-build-verify.ps1`
  (`/W4 /permissive- /WX`); `cs_assert_qt_free` held.
- Completes the R2 backend/analysis layer: audio, captions, cursor, auto-zoom,
  emphasis, and now cut-suggestion cores, plus the whisper + RNNoise real engines.
- Information-preserving overlap policy (no lossy cross-reason merge) leaves the
  final dedup/precedence to the editor at apply time.
- No new third-party dependency; `legal/OSS_BOM.csv` unchanged.
