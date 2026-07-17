# R1-05 Effects, Titles, and Basic Captions Design

Date: 2026-07-17
Status: approved under the user's standing instruction to follow the recommended
full-product direction without pausing for routine approval

## Outcome and scope

R1-05 implements delivery item 5 of the approved R1 design. A user can select a
timeline clip, set a visual transform or picture-in-picture preset, adjust audio
gain and fades, add/edit/remove a title, and add/edit/remove basic caption cues.
Every operation is durable, undoable, visible in preview, and exact after a
`.cstudio` reopen.

This is not an MVP boundary. Scene/source Studio workflow, markers, shortcuts,
recording HUD, export, and long-form acceptance remain delivery items 6-8 and
continue after this slice.

## Evidence and commercial OSS boundary

The pinned MLT 7.40.0 runtime currently allows only `core` and `avformat`.
Those modules already provide decoding, playlists, crop/resize/composite, audio
conversion, and mixing.

The broader MLT modules are not enabled wholesale:

- `normalize/filter_volume.c` and its service metadata declare GPLv2;
- the `plus` module advertises LGPL services but compiles headers and subtitle
  sources that declare the GNU GPL, so the whole module is not accepted without
  a separate legal/source-level audit;
- `qt6` would add another MLT plugin and Qt SvgWidgets/runtime surface merely to
  duplicate text rendering already available in the application.

R1-05 therefore reuses audited MLT core/avformat and the already-used Qt Gui
stack. Small Creator Studio-owned image/audio processors fill only the missing
transform and envelope behavior. No external `melt` process, GPL MLT module, or
MLT XML project state is introduced. This is an engineering distribution
decision; final legal approval remains an R4 release gate.

## Approaches considered

### Chosen: durable domain plus derived render cache

SQLite and the Qt-free timeline own transforms, audio envelopes, title payloads,
and caption cues. Qt rasterizes generated text into content-addressed transparent
PNG cache files off the UI thread. MLT avformat reads those images and the
existing graph composites them. Creator Studio-owned MLT frame processors apply
per-clip image transforms and sample-accurate audio gain/fades.

This preserves exact undo/reopen semantics, keeps preview and later export on
one graph, and adds no unapproved runtime module.

### Rejected: enable all MLT plus/qt6 services

This offers ready-made affine/text services but broadens the runtime and mixes
license declarations that are not acceptable for the closed-source commercial
assumption without further legal review. The GPL volume implementation remains
unusable even with these modules.

### Rejected: move effects and text to an FFmpeg filter graph

FFmpeg can implement the features, but it would duplicate MLT scheduling,
compositing, seeking, and render behavior. Preview and export could then diverge,
contradicting the selected R1 architecture.

## Domain model

### Existing values retained

`VisualTransform` remains canvas-normalized and continues to contain position,
size, scale, rotation, crop, opacity, and z-order. `AudioEnvelope` retains the
validated -96 dB through +24 dB gain and non-overlapping linear fade durations.
No raw MLT property string enters the domain.

PIP identity is derived, not stored:
`identifyPipPreset(transform, sourceAspect, canvasAspect)` returns a preset only
while every canonical value matches. Selecting a preset writes its complete
transform. A later manual change naturally returns `Custom`. This avoids a
schema column whose value could disagree with the actual geometry.

The initial preset set is deterministic:

- `FullFrame`;
- `TopLeft`, `TopRight`, `BottomLeft`, and `BottomRight` at 30 percent canvas
  width, aspect-preserving height, and 4 percent safe-area margins.

### Generated visual values

Add a typed `CueId`, `TextAlignment`, canonical `RgbaColor`, `TitlePayload`, and
`CaptionCue`.

A title payload contains UTF-8 text, requested font family, normalized x/y
placement, canonical foreground/background RGBA, and alignment. Title text is
1-512 Unicode code points and the requested font family is 1-128 code points.

A caption cue contains a stable ID, start offset, positive duration, and UTF-8
text of 1-2,000 Unicode code points. Cues are sorted by `(start, id)`, must stay
inside their caption clip, and may not overlap within one caption clip.

`Clip` gains validated `createTitle` and `createCaption` constructors. Generated
clips have no media asset ID. Their source range is canonical zero-based time
with the same duration as their timeline range. A title clip owns exactly one
title payload. A caption clip owns one or more cues. Asset, title, and caption
payloads are mutually exclusive.

### Track creation

Existing projects contain video/audio tracks only. The first title or caption
command atomically creates a stable `title-1` or `caption-1` track when missing,
then inserts the generated clip. Undo removes both the clip and a track created
by that same command. A pre-existing empty/generated track is never removed by
undo. This requires validated `Timeline::removeTrack` support but no schema
migration.

## Durable commands and persistence

The command set adds:

- `SetVisualTransformCommand`;
- `SetAudioEnvelopeCommand`;
- `AddTitleCommand`, `EditTitleCommand`, and `RemoveGeneratedClipCommand`;
- `AddCaptionCueCommand`, `EditCaptionCueCommand`, and
  `RemoveCaptionCueCommand`.

Each command owns the minimal previous value needed for exact undo. Adding a
generated clip may also own the track it created. Failed validation changes no
timeline state and enters no history. All command payload and undo JSON is
canonical, bounded, rejects unknown fields, and is rehydrated on reopen.

Migration 002 already contains `clip_visual_transforms`,
`clip_audio_envelopes`, `titles`, and `caption_cues`; R1-05 does not rewrite the
checksum-pinned migration or add a new migration. `SqliteTimelineStore` extends
its existing snapshot transaction to read/write TITLE and CAPTION clips and
their payload tables. Command commit, snapshot replacement, history event, and
checkpoint remain one SQLite transaction.

Edits use the R1-04 worker sequence:

1. validate and execute against staged domain/history state;
2. commit domain snapshot, command event, and checkpoint atomically;
3. publish committed models;
4. synchronize the derived text cache and MLT graph.

## Generated text cache

`GeneratedOverlayCache` lives in the Qt application layer and runs on the editor
session thread. It uses `QImage`, `QPainter`, `QTextLayout`, and `QSaveFile`; it
never runs on the UI thread.

The cache key is SHA-256 over a canonical payload containing clip/cue identity,
text/style, exact canvas size, frame rate, and resolved font family. Output is a
transparent, premultiplied RGBA PNG under:

`cache/generated/<sha256>.png`

The database remains the source of truth. Missing or corrupt cache entries are
regenerated on project open. Abandoned temporary files are removed safely, but
content-addressed completed PNGs are not deleted while the application may hold
an MLT graph. Bounded cache garbage collection is deferred to the R4 storage
maintenance gate rather than risking deletion under an active producer.

`TimelineSnapshot` gains Qt-free generated-overlay descriptors that map a title
clip or caption cue to its validated package-relative raster path and timeline
range. It also carries the validated manifest canvas width/height so normalized
PIP geometry and text rasterization are independent of preview/export
resolution. The MLT graph receives paths and scalar values, not Qt objects.

Cache generation is derived work after a durable commit. If rasterization fails,
the worker still returns the committed timeline and a diagnostic. The overlay is
explicitly unavailable, preview becomes stale/diagnostic, and reopen retries
generation. A cache failure never pretends the edit rolled back.

Font lookup first requests the stored family and otherwise uses Qt's platform
sans-serif fallback. The resolved family participates in the cache key and is
shown in diagnostics. Bundling a metrically fixed Korean/Latin OSS font and its
OFL evidence remains an R4 packaging decision; the R1-05 acceptance records the
font resolved on this machine.

## MLT graph behavior

Audio keeps one playlist per domain audio track. Each visual asset clip, title,
and caption cue becomes a native overlay branch so its constant transform and
z-order are exact even when different clips on one domain track use different
values. Branch order is deterministic by `(zOrder, domain track position,
timeline start, clip/cue ID)`. The physical acceptance measures the resulting
30-minute graph; if the correctness-first branch layout misses the R1 resource
budget, lane packing of non-overlapping branches is completed inside R1-05 rather
than deferred.

### Visual processing

The Creator Studio image processor is attached to each visual producer and uses
inverse affine mapping with bounded
bilinear sampling into the profile canvas. It applies crop, scale, rotation,
normalized position/size, opacity, and transparent padding. An identity
transform bypasses allocation. The existing audited core composite transition
then layers the processed branches in the deterministic order above.

Generated PNGs are held for their exact clip/cue duration through avformat image
producers. Missing derived overlays produce transparent frames plus a diagnostic,
not a dropped durable clip.

### Audio processing

The Creator Studio audio processor requests float PCM, converts dB to a finite
linear multiplier, and applies gain plus sample-accurate linear fade-in/fade-out
using the clip-local sample position. It clamps output to the float PCM range and
never reads outside a block. Existing `audioconvert` and core `mix` remain the
format and track-mix implementation.

The same processing path is used by preview now and the revision-frozen export
graph in R1-07, preventing preview/export effect drift.

## Application and QML behavior

The selected-clip Inspector becomes model-driven sections:

- Visual: x/y/width/height, scale, rotation, crop, opacity, z-order, reset, and
  five PIP preset buttons;
- Audio: gain dB, fade-in, fade-out, and reset;
- Title: add at the playhead with a default three-second duration, edit text and
  style, and remove;
- Captions: create/reuse a caption clip, add a default two-second cue at the
  playhead, edit cue start/duration/text, and remove.

Controls are enabled only for compatible selected clip/track kinds and while no
durable or engine command is busy. Values are submitted explicitly rather than
on every keystroke. Keyboard actions and visible controls call the same
controller methods. The timeline delegate identifies TITLE/CAPTION clips and the
Inspector reports the resolved/custom PIP state.

All controller input is converted to typed domain values before it reaches the
worker. Invalid numbers, invalid color strings, empty/overlong text, overlapping
cues, locked tracks, and selection that became stale return a visible error and
do not issue an engine update.

## Concurrency and error behavior

- SQLite, text layout/rasterization, cache I/O, and MLT processing stay off the
  UI thread.
- At most one durable command is active; MLT update ordering remains revision
  sequential.
- A durable failure changes neither model, cache descriptor, nor MLT graph.
- A post-commit cache or engine failure preserves the edit, marks preview stale,
  and retries a full rebuild from the committed snapshot.
- Old generation cache/engine results are ignored.
- Offline source media, missing generated cache, and unavailable font fallback
  remain represented with entity-specific diagnostics.

## Verification

### Domain and persistence

- validation boundaries for transform, preset recognition, colors, title text,
  cue text/ranges, and cue overlap;
- execute/undo/redo byte-equivalent round trips for every new command;
- title/caption track auto-create and exact undo behavior;
- canonical JSON decode rejects unknown, malformed, contradictory, or future
  payloads;
- real SQLite reopen preserves generated clips, effects, history cursor, clean
  state, and Unicode Korean text.

### Derived cache and MLT

- deterministic cache keys and pixel-identical transparent PNG output;
- missing/corrupt cache regeneration and abandoned-temporary-file cleanup;
- real MLT frames prove PIP position/size/crop/opacity/rotation;
- real PCM proves gain and both fades at exact sample windows;
- title and overlapping caption-time fixtures prove expected visible pixels;
- no GPL, extra, changed-hash, or unexpected runtime module is staged.

### Controller and UI

- compatible/incompatible selection state and explicit-submit semantics;
- commit-before-cache/engine ordering and post-commit failure recovery;
- QML object/shortcut parity, Unicode text, bounds, accessibility names, and
  disabled/busy states;
- UI event-loop responsiveness during rasterization and graph rebuild.

### Physical acceptance

On a real Unicode package: set camera PIP with rotation/crop/opacity, set
microphone gain and fades, add a Korean title and multiple Korean caption cues,
undo/redo across effect and text commands, save, destroy every worker/engine,
reopen, and compare domain state plus preview pixels/audio samples. Repeat the
workflow with an injected cache failure and SQLite commit failure to prove their
different durability boundaries.

The final gate is a clean MSVC `/W4 /permissive- /WX` audited-MLT build, complete
sequential CTest with zero skips/failures, repeated physical acceptance,
`creator_studio.exe` responsiveness, and a product link/runtime audit excluding
test libraries and unapproved modules.
