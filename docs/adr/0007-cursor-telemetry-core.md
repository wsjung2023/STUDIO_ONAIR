# ADR-0007: Cursor Telemetry Core (R2-01)

## Status
Accepted (gate-free core; OS capture hook, durable recorder/migration, and editor
features deferred)

## Context
R2-01 ("커서 좌표·클릭 이벤트 소스 분리 기록") records the cursor as a
source-separated capture stream with project-timebase timestamps, feeding later
click-emphasis / cursor-hide / auto-zoom features. Codex authored a design
(`docs/superpowers/specs/2026-07-18-r2-01-cursor-telemetry-design.md`) covering the
full feature. This slice implements the gate-free CORE, consistent with that
design's Qt-free value-type + `ICursorSource` + deterministic-fake architecture,
built parallel-safe on the R1 tip.

## Decision
New Qt-free module `cs_cursor` (`src/cursor/`, DEPENDS `cs_core`, `cs_domain`,
`nlohmann_json`):

- `CursorPoint` (normalized x,y ∈ [0,1]), `CursorButton` {Left,Right,Middle},
  `CursorMoveEvent` / `CursorClickEvent` — validated value objects with
  project-timebase `TimestampNs`.
- `CursorNormalizer` — raw pixel + source WxH → normalized `CursorPoint` (rejects
  zero dims, clamps out-of-frame).
- `ICursorSource` — pull port over raw move/click samples; the real OS hook slots
  behind it later.
- `FakeCursorSource` — deterministic scripted source (no clock/RNG/OS).
- `CursorEventSerializer` → schema-valid `cursor.move` / `cursor.click`
  (`schemas/event.schema.json`, already defined); `CursorNdjsonSink` — RAII
  append-only NDJSON to the package, never the DB (§6).

**Output contract is the schema.** Where Codex's design's internal persistence
model (integer parts-per-million coordinates, header/geometry NDJSON, SQLite
migration 005) diverged from `event.schema.json` (normalized float [0,1]
`cursor.move`/`cursor.click`), the core was implemented **to the schema**.

## Deferred (explicit)
- Real OS cursor capture (Windows Raw Input / low-level mouse hook = platform
  capture, like the camera/mic backends).
- Durable recorder + coalescing queue + SQLite store / migration 005 (Codex's
  design; a DB migration must be coordinated for its number when it lands).
- Click-emphasis render, cursor hide/replace, auto-zoom candidate generation and
  zoom-region editing (R2-02 / R2-03) — editor/app territory.

## Consequences
- 21 module tests / 698 total green via `scripts/studio-build-verify.ps1`
  (`/W4 /permissive- /WX`); `cs_assert_qt_free` held; both cursor events validate
  against the committed schema.
- No new third-party dependency; `legal/OSS_BOM.csv` unchanged.
- When the durable recorder lands it will add a DB migration — the number must be
  reconciled with any concurrent R1/R2 migration (coordination note).
