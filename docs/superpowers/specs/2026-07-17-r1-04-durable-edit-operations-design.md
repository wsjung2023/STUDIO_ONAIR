# R1-04 Durable Edit Operations Design

## Status and scope

This slice implements delivery item 4 of the approved R1 design: selectable
timeline clips, split, leading/trailing trim, lift delete, ripple delete,
durable undo/redo, explicit save checkpoints, and exact project reopen. It does
not reduce the product roadmap. Visual transforms, audio envelopes, titles, and
captions remain the next R1 slice; Studio-to-timeline recording reconciliation
remains part of the approved Studio workflow slice.

## Chosen architecture

`EditorController` remains the UI owner and keeps the existing asynchronous MLT
engine worker. A second `EditorSessionWorker` owns the current
`SqliteTimelineStore` and `TimelineEditService` on a dedicated thread. No
SQLite, command replay, or filesystem package work runs on the UI thread.

Opening a project validates `manifest.json`, resolves the package-owned database
without accepting a link or path escape, opens the timeline store against the
manifest project identity, and loads the durable edit session. If the project
has no primary timeline, the worker atomically creates a 60 fps `Main` timeline
with unlocked video and audio tracks before loading it. Existing timelines are
never replaced or silently repaired.

The worker accepts product edit requests rather than QML or MLT concepts:

- split one selected clip at the playhead;
- trim the selected clip's leading or trailing edge to the playhead;
- delete a marked half-open time range with ripple disabled or enabled;
- undo, redo, and mark an explicit save checkpoint.

It constructs the existing domain commands with generated stable identities,
executes them through `TimelineEditService`, and returns only after the SQLite
transaction succeeds. A successful result contains the committed assets,
timeline, revision, history capabilities, clean state, and an engine
`TimelineChangeSet`. A failure contains an `AppError` and leaves the published
UI timeline and MLT graph unchanged.

## Controller state and flow

`EditorController` gains durable-session state: selected track/clip, optional
range-in and range-out markers, `canUndo`, `canRedo`, and `clean`. Clip
selection is identity-based and is cleared if a committed edit removes that
clip. Range markers are ordered, clamped to the current timeline, and form a
strictly positive half-open range.

The successful command order is fixed:

1. the worker commits the domain command and command-log event;
2. the controller publishes the committed media and timeline models;
3. the controller updates selection/history/clean properties;
4. the controller submits the revisioned change set to MLT;
5. an incremental MLT failure marks preview stale and schedules a full graph
   rebuild without rolling back the durable edit.

Only one durable edit command is in flight. Playback is paused before an edit.
Edit controls are disabled while the session worker or engine is busy, while
preview is stale, or when selection/range preconditions are not satisfied.

## User interface

Clicking a clip selects it visibly. The toolbar provides Split, Trim Start,
Trim End, Undo, Redo, and Save. The timeline range bar provides Set In, Set Out,
Lift Delete, and Ripple Delete. Buttons and keyboard shortcuts call the same
controller methods; shortcuts do not bypass enablement or validation.

The Inspector shows the selected clip identity, source/timeline bounds, and a
short actionable explanation when an operation is unavailable. Destructive
range operations show their exact marked duration before execution. This slice
does not add free-form numeric editing that can bypass domain validation.

## Error and recovery rules

- Invalid selection, boundary, or range returns a visible validation error and
  does not enqueue work.
- A durable commit failure leaves models, revision, history cursor, selection,
  and preview graph unchanged.
- A late result from a replaced project session is discarded by generation.
- Corrupt history, inconsistent project identity, redirected database files,
  and unsupported future state fail closed with entity context.
- Reopen restores the exact materialized timeline, revision, history cursor,
  clean checkpoint, and undo/redo capabilities before MLT is loaded.

## Verification

Tests cover controller preconditions, one-command backpressure, stale session
results, domain error propagation, selection survival/clearing, range marker
ordering, shortcut/button equivalence, and MLT update/rebuild behavior. Physical
SQLite acceptance creates a Unicode project package, performs split, trim,
lift, ripple, undo, redo, and save, destroys all controllers, reopens the
package, and proves the exact timeline/revision/history/clean state. Failure
injection proves a rejected transaction changes neither UI nor engine state.

The slice passes only after a warning-free clean build, the complete sequential
test suite, real package reopen acceptance, and application responsiveness
verification.
