# ADR-0009: Cursor Emphasis Plan Core (R2-02)

## Status
Accepted (gate-free plan core; render/compositing deferred)

## Context
R2-02 ("클릭 강조·커서 숨김/교체") makes clicks visible and hides the cursor when
idle, from recorded cursor telemetry (R2-01). The plan GENERATION is a pure
deterministic algorithm producing editable directives; the actual highlight/hide
render is an editor/compositor concern (deferred).

## Decision
New Qt-free module `cs_cursor_emphasis` (`src/cursor_emphasis/`, DEPENDS
`cs_cursor`, `cs_core`, `cs_domain`, `nlohmann_json`):

- `ClickEmphasis` — directive: position (`CursorPoint`), start `TimestampNs`,
  `DurationNs`, `CursorButton`, `EmphasisStyle` {Ripple, Highlight}, radius.
- `CursorHideSpan` — `domain::TimeRange` + `HideReason` {Idle, ExplicitRegion}.
- `EmphasisPlanParameters` — validated tunables (click duration 600 ms, radius
  0.06, idle threshold 2 s, min-movement radius 0.01).
- `EmphasisPlan` — ordered clicks + ordered, strictly non-overlapping hide spans.
- `EmphasisPlanner` — one `ClickEmphasis` per click; idle detection via a settle
  anchor (jitter within min-movement radius does not reset the idle run; a run
  ≥ idle threshold becomes a `CursorHideSpan{Idle}`; runs partition samples so
  spans are inherently ordered/non-overlapping).
- `EmphasisPlanSerializer` + `schemas/emphasis_plan.schema.json` (v1).

## Deferred (explicit)
- The actual click-highlight/ripple and cursor-hide render/compositing (editor +
  MLT, Codex R1); cursor replacement art; live capture-time emphasis.

## Consequences
- 29 module tests / 753 total green via `scripts/studio-build-verify.ps1`
  (`/W4 /permissive- /WX`); `cs_assert_qt_free` held.
- Completes the cursor-intelligence family (telemetry → auto-zoom → emphasis) as
  pure analysis cores on one branch; render/edit integration awaits R1.
- No new third-party dependency; `legal/OSS_BOM.csv` unchanged.
