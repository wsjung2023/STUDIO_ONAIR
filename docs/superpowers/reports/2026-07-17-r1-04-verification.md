# R1-04 Durable Edit Operations Verification

Date: 2026-07-17
Scope: R1 delivery item 4 only. R1 remains incomplete until the remaining
Studio/timeline reconciliation, text/caption, export, and physical long-run
gates pass.

## Delivered behavior

- `EditorSessionWorker` owns package validation, SQLite timeline persistence,
  command replay, and edit history on a dedicated thread.
- Split, leading/trailing trim, lift, ripple delete, undo, redo, and explicit
  save publish only committed durable state to the controller.
- Split/trim use bounded incremental MLT updates. Delete/undo/redo use an
  explicit full rebuild, avoiding the 256-track incremental-change bound after
  a successful durable commit.
- Selection, marked range, selected clip bounds, keyboard shortcuts, clean
  state, and Undo/Redo capabilities are model-driven QML state.
- A newly opened or recovered project enters the same tested editor binding.
- A Unicode `.cstudio` package reopens with the exact committed timeline,
  revision, history cursor, clean state, and Undo/Redo capabilities.

## Atomicity and review findings

- An actual SQLite trigger forces `edit_commands` insertion failure. The test
  proves that controller rows, selection, playhead, revision, clean state,
  preview-engine call count, and stored timeline remain unchanged.
- A 257-track delete proves that post-commit change publication cannot fail on
  the edit-engine affected-track bound.
- Final self-review found and corrected three observable gaps before merge:
  selected clip bounds were absent from the Inspector; opening a replacement
  project did not immediately invalidate transient selection/range/preview
  state; and starting a new range after an old Out marker could delete a wider
  interval than selected. Each correction was reproduced by a failing test
  before implementation.
- The physical workflow now asserts the second ripple range is exactly
  4.5-5.5 seconds rather than merely comparing whatever state was persisted.

## Audited Windows gate

- Configuration: MSVC Debug, `/W4 /permissive- /WX`, Qt 6.8.3, audited MLT
  7.40.0 runtime enabled.
- Fresh audit build: **261/261 steps passed**. The final review change rebuilt
  and relinked all affected product/acceptance targets in **10/10 steps**.
- Complete sequential CTest: **497/497 passed**, zero failures, in 67.50
  seconds.
- Final durable reopen/atomicity acceptance repeat: **10/10 passed**.
- Existing physical MLT Unicode preview/tamper acceptance remained part of the
  complete gate: **2/2 passed**.
- `creator_studio.exe` launched from the audited output with
  `Responding=True`.
- The product link command contains `cs_app` and `cs_mlt_adapter` and does not
  contain the test-only `cs_fakes` library.
- `git diff --check` passed before each implementation/review commit.

## Deliberate implementation deviations

- Delete-range right-side clip identities are generated for every spanning
  clip on an unlocked track, including disabled clips, because the domain
  command edits those clips as well. Restricting generation to enabled clips
  would make a valid disabled spanning clip fail atomically.
- Delete/undo/redo publish a full MLT rebuild instead of enumerating every
  affected track. This preserves post-commit publication for timelines larger
  than the engine's bounded incremental track list.
- `TimelineSnapshot::mediaRoot` is the implemented package media root contract;
  the earlier plan's `packageRoot` wording referred to the same path.

## Remaining scope

- R1-05 remains: Studio scene/source state and completed recordings must be
  reconciled into the editable timeline, followed by text/basic captions,
  render/export, and the complete R1 physical recording-edit-export gate.
- macOS execution is not verified on this Windows machine.
- Signing, installer/deployment layout, codec patent/royalty review, and release
  soak testing remain R4 gates.
