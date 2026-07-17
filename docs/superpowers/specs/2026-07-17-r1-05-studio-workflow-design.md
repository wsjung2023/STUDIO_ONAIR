# R1-05 Studio Scene, Source, and Recording Integration Design

## Scope and product boundary

R1-05 implements delivery item 5 of the approved R1 design without reducing the
product to a UI-only scene mockup. A user can persist project scenes, arrange
screen and camera sources, switch scenes while recording, place recording
markers, use configurable shortcuts, monitor a truthful recording HUD, and see a
completed source-separated recording appear as editable timeline clips.

R1-05 does not add export. Revision-frozen H.264 export is R1-06, and the final
30-minute capture-edit-export gate is R1-07. R1-05 nevertheless performs physical
recording-to-timeline acceptance so those later items do not inherit an untested
import boundary.

The first physically verified target remains Windows x64. macOS architecture and
native capture code are retained, but this Windows machine cannot claim Apple
framework or device acceptance.

## Chosen approach

The recorder continues writing independent screen, camera, microphone, and
system-audio segments. Studio persists scene snapshots, scene activation events,
recording markers, and the concrete capture-source roles used by each session.
After recording persistence completes, a deterministic reconciler probes the
published media and atomically appends assets, clips, transforms, audio enables,
and markers to the primary timeline.

This is preferred over recording only a precomposited program track because it
preserves lossless editorial freedom and avoids a second video encode. It is
preferred over a UI-only scene selector because every accepted scene switch must
survive reopen and affect the editable/final timeline.

## Architecture

```text
StudioPage Actions / Shortcuts
              |
              v
StudioWorkflowController ---- LiveRecordingController
              |                        |
              v                        v
StudioWorkflowWorker          source-separated recorder
              |                        |
              +-----------+------------+
                          v
                 SQLite migration 003
        scenes / scene_sources / recording events
                          |
                          v
               RecordingTimelineReconciler
                 |                    |
                 v                    v
          IMediaProbe             ITimelineStore
                 |                    |
                 v                    v
       audited FFmpeg adapter     assets/tracks/clips/markers
                                      |
                                      v
                             EditorController reopen
```

The workflow controller owns no native capture or FFmpeg object. Database,
probing, reconciliation, and settings I/O never run on the UI thread. QML receives
immutable list models and calls controller methods or shared `Action` objects; it
does not mutate domain values.

## Domain values

### Studio sources

`StudioSourceRole` has exactly four R1 values: `Screen`, `Camera`, `Microphone`,
and `SystemAudio`. R3 can add an avatar role through a later migration without
changing these stored values.

`SceneSource` contains:

- stable logical `SourceId`, independent of a native device handle;
- role, display name, stable order, and enabled state;
- optional `VisualTransform` for screen/camera;
- no visual transform for microphone/system audio.

Each scene contains at most one source per R1 role. Video transforms reuse the
same normalized, bounded `VisualTransform` value already used by Editor so Studio
and final preview cannot disagree about crop, rotation, opacity, and z-order.

### Scenes

`StudioScene` contains typed `SceneId`, non-empty UTF-8 name, stable order, and
ordered sources. A project always has at least one scene. New projects seed three
useful scenes:

1. `Presentation`: screen full-frame, camera top-right PIP, both audio sources on;
2. `Screen`: screen full-frame, camera off, both audio sources on;
3. `Camera`: camera full-frame, screen off, both audio sources on.

The user may add, duplicate, rename, delete, and reorder scenes while idle. The
last scene cannot be deleted. Scene/source structure and transforms are locked
while recording; scene switching and marker placement remain enabled. This makes
each recorded activation refer to an immutable scene snapshot instead of an
ambiguous object edited mid-take.

### Recording events and markers

`RecordingSceneEvent` stores session ID, monotonically increasing sequence,
scene ID, and non-negative position relative to recording start. Sequence zero at
position zero is persisted before recording is reported ready.

`RecordingMarker` stores marker ID, session ID, relative position, and UTF-8
label. Empty labels are valid and rendered as numbered markers. Markers are
recording-relative until reconciliation; the reconciler offsets them by the
timeline append position and creates durable `TimelineMarker` values.

## Migration 003

Migration 003 is checksum-pinned, transactional, contiguous after migration 002,
and creates:

- `scenes(scene_id, project_id, name, position, created_at_utc)`;
- `scene_sources(scene_id, source_id, role, position, enabled, transform fields)`;
- `studio_state(project_id, active_scene_id)`;
- `recording_sources(session_id, source_id, role)`;
- `recording_scene_events(session_id, sequence, scene_id, position_ns)`;
- `recording_markers(marker_id, session_id, position_ns, label)`;
- `recording_imports(session_id, timeline_id, base_ns, imported_revision,
  imported_at_utc)`.

Foreign keys tie every row to its project, session, or scene. Role, ordering,
finite normalized transform, crop-sum, opacity, z-order, time, and uniqueness
constraints are explicit. `recording_imports.session_id` is the idempotency key:
an already imported session is a successful no-op, not duplicate media.

Opening a migration-002 project seeds default scenes without modifying source
media or the existing timeline. A failed migration leaves the package at version
2. Checksum mismatch, partial-schema impostors, noncontiguous metadata, and retry
are rejected by the existing migration runner rules.

## Controller and recording lifecycle

`StudioWorkflowController` exposes scene/source models, selected and active IDs,
selected source transform, shortcut strings, marker count, reconciliation state,
and a status diagnostic. It uses a dedicated `StudioWorkflowWorker` thread.

Project open loads or seeds scenes. Scene edits follow stage → validate → SQLite
transaction → publish through explicit add, duplicate, rename, remove, reorder,
set-source, and switch-scene command objects. On failure no UI snapshot changes.
Selecting a scene while idle first persists `studio_state`. Switching while
recording first persists a `RecordingSceneEvent`; only success changes the active
live preview.

`LiveRecordingController` gains immutable active-session identity, start time,
current relative recording position, and lifecycle signals. The binding performs:

1. `ProjectController::begin` creates the recording session row;
2. an injected preparation callback persists active source ID-to-role mappings
   and the active scene event at position zero;
3. after this succeeds, start the existing recording engine;
4. while recording, route scene switches and markers through the Studio worker;
5. after engine completion and `ProjectController::complete` success, reconcile
   the session;
6. emit `timelineReconciled` only after the import transaction commits, then
   reopen Editor from the package.

If the engine start fails, the session abort path removes uncommitted Studio
recording events in a follow-up Studio transaction after the durable session
abort succeeds. If recording completion succeeds but reconciliation fails,
published media and the completed
session remain intact; the UI reports a recoverable failure and project reopen
retries every completed, unimported session.

## Media probing and reconciliation

`IMediaProbe` is a Qt-free port returning duration, video width/height/frame rate,
audio sample rate/channels, container/codec identity, byte size, and SHA-256 for a
package-owned path. `FfmpegMediaProbe` uses the already audited dynamic FFmpeg
libraries. It opens no external `ffprobe` process and adds no OSS component.

The reconciler validates every READY path below the package without following a
link outside it, probes the file, and creates deterministic IDs derived from
session/source/segment identity. It appends at the current timeline end.

For each segment:

- video creates one asset and one or more clips split at intersecting scene-event
  boundaries;
- each video subclip receives the scene's enabled flag and exact transform;
- audio creates one asset and clips split only where scene enable state changes;
- source ranges preserve exact offsets into the immutable segment;
- gaps or failed segments remain gaps and produce diagnostics rather than being
  silently stretched;
- recording markers become timeline markers at `appendBase + relativePosition`.

The full import, materialized timeline, assets, markers, edit checkpoint, and
`recording_imports` row commit in one SQLite transaction with one revision
advance. A fault at any statement leaves no partial assets, clips, or import row.
The timeline placement is recorded as one composite `ImportRecordingCommand` so
normal timeline undo/redo can remove or restore the imported clips and markers.
Assets and the `recording_imports` idempotency row remain durable on undo, keeping
the source media available in the media bin and preventing automatic re-import.

## Live preview compositor

Studio preview layers source items from the active immutable scene snapshot.
Screen and camera use bounded latest-frame mailboxes; preview consumption never
backpressures recording. A shared native video-preview base owns platform render
nodes, while `ScreenPreviewItem` and `CameraPreviewItem` only provide their
respective controller mailbox.

On macOS the existing zero-copy Metal/IOSurface path is reused for both video
roles. On unsupported Windows capture builds the UI remains explicitly labelled
unavailable; it never represents a test pattern as captured desktop or camera
content. Scene transforms, crop, opacity, visibility, and z-order are applied by
Qt Quick to the live preview and by the reconciler to timeline clips using the
same normalized values.

## Configurable shortcuts and action parity

Project-independent shortcut settings use injected `QSettings` storage and
validated `QKeySequence` strings. Defaults are:

- record/stop: `Ctrl+Shift+R`;
- add marker: `M`;
- previous scene: `Ctrl+PgUp`;
- next scene: `Ctrl+PgDown`;
- direct scenes 1–9: `Ctrl+1` through `Ctrl+9`.

Empty, invalid, duplicate, or application-reserved sequences (`Ctrl+Q`, `Ctrl+S`,
`Ctrl+Z`, `Ctrl+Shift+Z`, `Ctrl+O`, and `Ctrl+N`) are rejected before settings
change. `Action` objects are the single invocation boundary shared by
buttons, menu items, and `Shortcut`; tests therefore prove visible and keyboard
paths call the same controller command. Shortcuts are active only on Studio and
follow the same idle/recording state guards as visible controls.

## Studio UI and HUD

The left panel becomes real scene and source models with add, duplicate, rename,
delete, reorder, visibility, and active-scene controls. The right inspector edits
the selected video source using numeric validation and PIP presets. During
recording it becomes read-only while scene switching stays available.

The bottom HUD retains truthful device and recorder values and adds active scene,
relative recording duration, marker count, session identity suffix, microphone
and system-audio levels, capture and recording drops, queue pressure, sync drift,
disk space, encoder identity, reconciliation progress, and recoverable/terminal
diagnostics. No unavailable value is displayed as zero success; it is labelled
`Not active`, `Checking`, or with the actual failure.

All interactive controls have accessible names/descriptions, deterministic tab
order, keyboard activation, disabled-state explanation, and layout coverage at
1280×720, 1440×900, and 200 percent scale.

## Error, concurrency, and resource rules

- Scene/source edits are serialized on one project worker generation.
- Stale callbacks from a closed/reopened project are ignored by generation.
- Native capture, database, probe, and reconciliation work never blocks UI.
- Scene activation is not published until its recording event is durable.
- Reconciliation is idempotent and retried only for completed unimported sessions.
- Symlinks/reparse points, traversal, replaced files, and unsupported media fail
  closed with the recording retained.
- File, SQLite, FFmpeg, capture, mailbox, timer, thread, and render resources must
  be released before package cleanup succeeds on Windows.
- Repeated command failure changes neither published scene state nor timeline.

## Commercial OSS boundary

R1-05 adds no third-party library. It reuses Qt Quick/Gui, SQLite, the existing
audited FFmpeg dynamic build for in-process probing, and existing capture/MLT
boundaries. No external `ffprobe`/`melt`, GPL MLT module, GPL-enabled FFmpeg, web
runtime, account service, or cloud dependency is introduced. Engineering license
evidence remains subject to R4 legal and packaging review.

## Verification gates

### Domain and persistence

- scene/source validation, exact equality, and transform reuse;
- migration 1→2→3, fresh database 0→1→2→3, checksum mismatch, interruption, and retry;
- scene CRUD/order and recording event/marker atomicity;
- default scene seed idempotency and Unicode paths/names;
- recording import rollback at every statement and exact idempotent retry.

### Application and UI

- controller generation, busy/error, idle/recording guards, and stale callbacks;
- shortcut validation, persistence, conflicts, and Action parity;
- scene switch persistence before publish and marker timestamp correctness;
- QML empty/loading/ready/recording/reconciling/failure states;
- real accessibility tree checks, keyboard traversal, and target layouts;
- event-loop maximum gap below 250 ms during load and reconciliation.

### Physical workflow

A Unicode package receives a deterministic multi-source recording fixture with
screen, camera, microphone, and system audio segments, at least three scene
switches and five markers. After every engine/controller/store is destroyed, the
project reopens with exact scene state, one idempotent recording import, expected
tracks/assets/split clips/transforms/markers, real MLT pixels and mixed PCM, and a
removable package directory. Injected persistence, probe, missing-media, and
reconciliation failures repeat without partial imports or locked files.

The fresh Windows `/W4 /permissive- /utf-8 /WX` build, complete sequential CTest,
shipping dependency/runtime audit, app responsiveness probe, placeholder scan,
and independent review remain mandatory before R1-05 is marked complete.
