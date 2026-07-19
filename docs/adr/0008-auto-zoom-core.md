# ADR-0008: Auto-Zoom Candidate Core (R2-03)

## Status
Accepted (gate-free analysis core; zoom editing UI and render deferred)

## Context
R2-03 ("자동 줌 후보·줌 구간 편집") turns recorded cursor telemetry (R2-01) into
suggested zoom regions the creator can accept/edit — a screen-recording-specific
productivity feature. The candidate GENERATION is a pure deterministic algorithm
over cursor events, so it is built parallel-safe and gate-free; the editing UI and
the actual zoom render are editor/compositor concerns (deferred).

## Decision
New Qt-free module `cs_autozoom` (`src/autozoom/`, DEPENDS `cs_cursor`, `cs_core`,
`cs_domain`, `nlohmann_json`):

- `ZoomRegion` — normalized target: center (`cursor::CursorPoint`) + zoom factor
  (≥1); viewport derived and validated ⊆ [0,1].
- `ZoomCandidate` — the editable suggestion: `domain::TimeRange` (in→hold→out,
  project timebase) + `ZoomRegion` + score ∈ [0,1].
- `AutoZoomParameters` — validated tunables (dwell 800 ms, focus radius 0.08, max
  factor 2.5, min gap 500 ms, click weight 0.15).
- `AutoZoomAnalyzer` — the algorithm: grow a dwell while the cursor stays within
  `focusRadius` of the running centroid; a roam beyond it closes the dwell (zoom
  out); a dwell ≥ `minDwellDuration` or with ≥2 clicks becomes a candidate centered
  on the centroid, factor scaled by tightness, scored by dwell length + tightness +
  weighted clicks; candidates within `minGap` merge; output is time-ordered and
  strictly non-overlapping.
- `ZoomCandidateSerializer` + `schemas/zoom_candidate.schema.json` (v1).

## Deferred (explicit)
- Zoom-region editing UI, zoom keyframe/animation, and the actual zoom
  render/compositing (editor + MLT compositor, Codex R1 territory).
- Live auto-zoom during capture.

## Consequences
- 26 module tests / 724 total green via `scripts/studio-build-verify.ps1`
  (`/W4 /permissive- /WX`); `cs_assert_qt_free` held.
- Input-fault surface is minimal by construction: the analyzer consumes
  already-valid cursor value objects, so it only needs to reject out-of-order input;
  finite/range guards live at the value-object boundaries.
- No new third-party dependency; `legal/OSS_BOM.csv` unchanged.
