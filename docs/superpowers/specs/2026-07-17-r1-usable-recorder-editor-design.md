# R1 Usable Recorder and Basic Editor Design

Date: 2026-07-17
Branch: `feat/r1-usable-recorder-editor`
Status: approved by the user on 2026-07-17

## Outcome and non-reduction rule

R1 delivers the complete `IMPLEMENTATION_ROADMAP.md` **Usable Recorder & Basic
Editor** phase. A user can record a real 30-minute tutorial, remove multiple
mistake ranges, reposition the camera, add a title and basic captions, and export
the result as H.264 MP4.

The numbered slices in this document are implementation and verification order,
not an MVP cut and not permission to stop early. R1 is complete only when all
Studio, Editor, Export, reliability, licensing, and acceptance requirements below
are implemented and verified. The resulting domain and adapter boundaries must
also remain suitable for the planned R2 captions/AI/cursor automation, R3 avatar,
and R4 commercialization/reliability/distribution phases.

## Product and distribution assumption

The first commercial form is a paid, closed-source Windows desktop application,
with macOS retained as a supported architecture and with service-ready engine
boundaries for a later cloud renderer. This assumption does not introduce a
network service or account system into R1.

Open-source components are preferred where they reduce risk or duplicated work,
provided that their code, binary, plugin, codec, model, and asset obligations are
compatible with commercial distribution. Legal approval remains a release gate;
this document is an engineering policy rather than legal advice.

## Fixed scope

### Studio

- scene and source panel;
- transform inspector;
- picture-in-picture presets;
- scene switching;
- configurable shortcuts;
- recording markers;
- recording-state HUD;
- preservation of separate screen, camera, microphone, and system-audio sources.

### Editor

- media bin populated from project-package assets;
- multi-track timeline;
- play, pause, seek, and frame-accurate playhead placement;
- cut/split, edge trim, range delete, and ripple delete;
- screen and camera transforms;
- audio gain and fade-in/fade-out;
- text titles and basic caption cues;
- undo and redo for every user edit;
- autosave without destructive source-media modification.

### Export

- H.264 MP4;
- 1080p and 4K presets;
- hardware-encoder capability probing;
- deterministic software fallback;
- progress, cancellation, and useful failure reporting;
- output validation before a render is reported complete.

## Chosen approach

Creator Studio owns a Qt-free domain timeline. MLT is a dynamic-library edit
engine behind `IEditEngine`; FFmpeg remains behind its existing adapter for media
inspection, codec capability probing, encode/mux operations not owned by MLT, and
recording. SQLite is the durable source of truth. MLT XML is a rebuildable render
cache only. OpenTimelineIO is an optional future interchange adapter, never the
native project format.

This approach is chosen over an FFmpeg-only NLE because implementing scheduling,
mixing, transition, seeking, and render-graph behavior again would delay the full
product. It is chosen over an external `melt` process because `melt` has different
licensing from the MLT core and because an opaque process protocol would weaken
preview, cancellation, and error control.

## Architecture

```text
QML Studio / Editor / Export
              |
              v
StudioController / EditorController / ExportController      Qt application layer
              |
              v
Commands -> Project + Timeline + Scene                      Qt-free domain layer
       |              |              |
       v              v              v
 ProjectStore     IEditEngine     IRenderJob                 Qt-free ports
       |              |              |
       v              +-------+------+
 SQLite adapter               v
                       MLT adapter -> approved MLT modules
                              |
                              +------> FFmpeg dynamic libraries
```

QML never receives an FFmpeg or MLT object and never mutates a domain object.
Controller methods validate UI input, create a command, execute it through the
application service, persist it, and then publish an immutable view model.

## Domain model

All time values use `core::TimestampNs` or `core::DurationNs`; frame rates use the
existing exact `core::FrameRate`. Raw unitless time integers are permitted only at
the SQLite serialization boundary.

### Typed identities

R1 adds `AssetId`, `TimelineId`, `TrackId`, `ClipId`, `SceneId`, `CommandId`, and
`RenderJobId` to the typed identifier family. IDs are stable across autosave,
undo/redo, migrations, and render-cache regeneration.

### MediaAsset

`MediaAsset` describes immutable source media:

- typed ID and media kind;
- package-relative path;
- source ID and recording-session provenance when recorded in Creator Studio;
- exact duration, video dimensions/frame rate, audio layout/sample rate;
- file size and content fingerprint used to detect replacement or corruption;
- availability state and optional proxy/thumbnail references.

External absolute paths are not stored directly in timeline clips. Import copies
or safely adopts media into the project package according to an explicit import
policy. Missing assets remain represented and produce a visible offline state.

### Timeline, Track, and Clip

`Timeline` owns ordered tracks and canvas settings. Tracks have a stable order,
kind, user-visible name, enabled/locked state, and a non-overlap invariant within
one ordinary video or audio track. Title and caption tracks may use their own cue
rules but still reject negative or zero durations.

Each `Clip` contains:

- asset identity;
- source start and source duration;
- timeline start and timeline duration;
- enabled state;
- visual transform for visual clips;
- audio envelope for audible clips;
- optional title or caption payload for generated clips.

The initial speed is 1:1, so source and timeline durations match. The model keeps
them distinct so a later speed/ramp feature does not require a destructive schema
rewrite. Clip ranges must remain inside the known asset duration.

### VisualTransform

Transforms are canvas-normalized values so 1080p and 4K renders share the same
layout:

- normalized position and size;
- scale, rotation, crop, opacity, and z-order;
- aspect-fit/fill mode;
- a preset identity only while the values still match the preset.

R1 uses constant transforms per clip. The representation leaves a keyframe
extension point for R2/R3 without pretending that keyframes are implemented.

### AudioEnvelope

R1 supports clip gain plus bounded linear fade-in and fade-out durations. Invalid
gain, non-finite values, overlapping fades beyond clip duration, and envelopes on
non-audio clips are rejected in the domain.

### Title and caption

A generated title clip stores UTF-8 text, font-family fallback request, normalized
placement, color, background, alignment, and duration. Basic caption cues store
start, duration, and text. R1 does not perform speech recognition; R2 can populate
the same caption track through a transcription command.

## Commands, undo, and autosave

Every mutation implements the repository's `ICommand` rule. Initial commands are:

- add/remove/reorder track;
- insert/remove/move clip;
- split clip;
- trim clip edge;
- range delete and ripple delete;
- set visual transform or PIP preset;
- set gain/fades;
- add/edit/remove title;
- add/edit/remove caption cue;
- add/remove marker;
- add/edit/switch scene.

A command records the minimal previous values required for exact undo. Composite
operations such as ripple delete execute atomically and undo atomically. A command
that fails validation changes neither memory nor storage and does not enter the
undo stack.

The SQLite transaction contains the materialized timeline update and command-log
entry. Autosave runs after a successful command on the project worker, never on
the UI thread. Explicit save writes a named checkpoint; autosave and explicit save
remain separate. The current undo cursor and clean checkpoint are persisted so a
crash/reopen does not silently present edited state as saved state. History may be
compacted only at an explicit checkpoint after retaining the materialized state
and enough metadata to explain the boundary.

## Project database migrations

Migration 002 adds the edit-domain state:

- `media_assets`;
- `timelines`;
- `tracks`;
- `clips`;
- `clip_visual_transforms`;
- `clip_audio_envelopes`;
- `titles` and `caption_cues`;
- `markers`;
- `edit_commands` and `edit_checkpoints`.

Migration 003 adds `scenes` and `scene_sources` with stable ordering and the
source transform/visibility state required by Studio. Migration 004 adds
`render_jobs` with the frozen timeline revision, preset, destination, state,
progress, fallback result, timestamps, and diagnostic error fields.

Tables use foreign keys, explicit checks, stable ordering keys, and project-owned
rows. Migration is transactional and checksum-pinned like migration 001. Opening
an older package upgrades it without changing source media. A failed migration
leaves the package at its previous schema version.

## Edit-engine port

The Qt-free port exposes product concepts rather than MLT concepts:

```cpp
class IEditEngine {
public:
    virtual Result<void> load(const TimelineSnapshot&) = 0;
    virtual Result<void> update(const TimelineChangeSet&) = 0;
    virtual Result<void> play() = 0;
    virtual Result<void> pause() = 0;
    virtual Result<void> seek(core::TimestampNs) = 0;
    virtual Result<PreviewFrame> requestFrame(core::TimestampNs) = 0;
    virtual Result<std::unique_ptr<IRenderJob>> render(const RenderRequest&) = 0;
    virtual ~IEditEngine() = default;
};
```

The implementation plan defines the value types used by this interface before the
adapter is implemented. The invariant contract is immutable timeline snapshots,
typed time, explicit errors, bounded work, and cancellable render jobs. Preview
callbacks are marshalled to Qt only in the application adapter. Decode, render,
database, thumbnail, and waveform work never runs on the UI thread.

## MLT adapter and commercial OSS boundary

MLT is pinned to an audited source revision and built as dynamic libraries. Build
configuration, source hash, dependency versions, produced binaries, and module
licenses are release artifacts.

The adapter must:

- disable uncontrolled plugin discovery;
- set a product-owned module/data path;
- load only a compiled and runtime-verified allowlist;
- reject missing, extra, wrong-version, or wrong-hash runtime modules;
- translate every MLT failure into `AppError` with diagnostic context;
- stop producers/consumers and release frames deterministically;
- rebuild its graph after an unsupported incremental change;
- never expose MLT XML as the project source of truth.

No module is allowlisted only because it ships with MLT. Its source files,
transitive dependencies, runtime metadata, and binaries must all be reviewed.
`melt`, GPL modules, GPL-enabled FFmpeg, and nonfree FFmpeg are excluded from the
closed-core package. Codec patent or royalty questions are tracked separately
from copyright licenses.

Qt modules are also audited individually; an LGPL-compatible Qt installation
does not imply that every Qt add-on is approved. Relinking rights, notices,
licenses, source offers where required, and installation information are retained
for R4 packaging. If an obligation cannot be met, the dependency is replaced or
a commercial license is obtained before distribution.

## Studio data flow

Studio scenes refer to persistent project sources, not native device handles. A
scene switch atomically applies source visibility and transform settings to the
live compositor while the R0 recorder continues writing source-separated tracks.
Markers are timestamped on `ProjectClock` and stored in the active timeline.

The HUD shows recording duration, active scene, microphone/system-audio levels,
dropped frames, queue pressure, disk space, and recoverable/terminal failures.
Shortcuts call the same controller commands as visible controls and are ignored
or rejected according to explicit recording state.

## Editor data flow

Opening a project performs these steps:

1. validate the package and migrate the database;
2. reconcile completed/recovered recording segments into immutable media assets;
3. load or create the primary timeline;
4. validate all asset paths without following links outside the package;
5. publish media-bin and timeline view models;
6. asynchronously construct the edit-engine graph;
7. show offline/unsupported media explicitly instead of dropping clips.

Edits update the domain and database first. The engine receives the committed
change set second. If engine synchronization fails, the durable edit remains and
the controller marks preview stale, rebuilds the graph, and reports the problem;
it does not roll back valid user work because a disposable cache failed.

## Export flow

An export request freezes a revisioned `TimelineSnapshot`, validates assets and
free space, chooses a preset, and creates a persistent render job. Later edits do
not change a running export.

Hardware encoders are selected only after a capability probe and a small encode
test. A hardware initialization or early encode failure retries once with the
audited software H.264 path when the preset permits fallback. Partial output is
written under an exclusive destination-local temporary name so final publication
stays on one volume, and is atomically published only after the container is
finalized and probed for expected video/audio streams and duration.

Cancellation is cooperative and bounded. It stops the consumer, closes media
resources, removes or quarantines the partial artifact, persists the cancelled
state, and never reports 100 percent. Progress is based on rendered timeline time,
not wall-clock guesses.

## Error and recovery behavior

- Missing media: keep the clip and show offline state; offer relink, do not delete.
- Unsupported codec: identify the asset and required capability.
- Corrupt project row: reject the project with table/entity context.
- Preview engine crash/failure: preserve edits and rebuild disposable engine state.
- Full disk: stop export safely and retain diagnostics; recording uses the existing
  safe-stop path.
- Application crash during edit: reopen the last committed command transaction and
  distinguish it from the last explicit save checkpoint.
- Application crash during export: mark the interrupted job failed/recoverable and
  quarantine its partial file.
- Hardware encoder failure: bounded software fallback with a visible diagnostic.

No error is only logged and ignored. User-facing messages are short; diagnostics
retain the causal error chain, component versions, and render revision.

## Verification strategy

### Domain and persistence

- property-style range tests for split, trim, move, delete, and ripple invariants;
- execute/undo/redo round trips restore byte-equivalent snapshots;
- composite-command rollback on injected failure;
- migration 001 to 002 success, checksum mismatch, interruption, and retry;
- reopen after every command type and at every undo cursor position;
- missing, replaced, symlinked, and out-of-package asset paths.

### Edit engine

- fake engine contract tests independent of MLT;
- deterministic domain-to-MLT graph fixtures;
- approved-module allowlist and unexpected-module rejection;
- seek/frame requests at start, clip boundaries, gaps, and final frame;
- repeated load/play/seek/stop cycles with resource-leak checks;
- mixed screen/camera/audio/title/caption render fixtures.

### UI and application

- controller state-machine and error-path tests;
- QML smoke tests for empty, loading, ready, offline-media, exporting, and failed
  states;
- shortcut and visible-control equivalence;
- UI responsiveness while loading, autosaving, seeking, and exporting;
- accessible names, keyboard navigation, and high-DPI layout coverage.

### Export

- 1080p and 4K preset validation;
- hardware success, rejection, mid-start failure, and software fallback;
- cancellation at startup, midpoint, and finalization;
- output stream/duration/frame-rate/audio checks using the audited probe;
- deterministic short golden renders within declared pixel/audio tolerances.

### R1 physical acceptance

R1 does not pass on unit tests alone. On supported hardware, record a 30-minute
screen tutorial with camera, microphone, and system audio; place markers and switch
scenes; delete multiple mistake ranges; move and resize the camera; adjust gain and
fades; add a title and caption cues; export 1080p and the supported 4K path; and
inspect A/V sync, output integrity, resource cleanup, and recovery diagnostics.

Windows acceptance is mandatory for the first commercial target. macOS native
acceptance remains an honest separate gate where the current machine cannot
provide Apple frameworks or devices.

## Delivery order and canonical milestone mapping

1. Timeline domain, migration 002, commands, durable undo/autosave.
2. Media-bin and multi-track Editor view models with a fake engine.
3. Audited MLT bootstrap, module allowlist, preview engine, and real playback.
4. Split/trim/delete/ripple UI and project reopen verification.
5. Transform, PIP, audio envelope, title, and basic caption editing.
6. Scene/source Studio workflow, shortcuts, markers, and recording HUD.
7. Revisioned H.264 export, presets, probing, fallback, progress, and cancellation.
8. Long-form acceptance, OSS evidence bundle, performance and recovery closure.

These are eight internal implementation checkpoints, not eight R1 milestones.
Checkpoints 4 and 5 together are canonical R1-04, checkpoint 6 is R1-05,
checkpoint 7 is R1-06, and checkpoint 8 is R1-07. There is no R1-08 stage.
Work continues through checkpoint 8 unless a real requirement, safety,
licensing, or external hardware blocker requires user action.

## R2-R4 continuity

- R2 transcription writes caption commands into the existing caption track;
  cursor automation writes typed effect/overlay commands rather than editing MLT
  XML.
- R3 avatar output is another scene source and timeline visual track through the
  same compositor/edit-engine ports.
- R4 packages the already-pinned OSS graph, notices, relinking material, diagnostics,
  updater, crash handling, and commercial service policies without replacing the
  project model.

This preserves the full roadmap rather than creating a disposable R1 editor.
