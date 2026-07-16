# R0-06 Synchronization and Recovery Design

Date: 2026-07-16
Branch: `feat/r0-06-sync-recovery`
Status: approved by the roadmap and the user's instruction to continue autonomously

## Outcome

R0-06 adds a project-clock coordinator in front of the R0-05 track recorders.
One live audio source is the master timeline. Other sources are measured against
an interpolated master position using only `ProjectClock`; wall time never enters
media synchronization. Video is aligned with an explicit drop/duplicate policy,
and non-master audio receives a tightly bounded resampling correction.

Interrupted recording recovery also becomes filesystem-aware. Package-local
`.part` files belonging to WRITING rows are moved to a no-overwrite quarantine
before those rows become FAILED. The sequence is idempotent after another crash.

## Fixed constraints

- Master choice is deterministic: microphone, then system audio, then the first
  video source if no live audio exists.
- Callback receipt timestamps use monotonic `ProjectClock`, never `Utc` or a wall clock.
- Drift is `source project timestamp - interpolated master timestamp`.
- A source's first measurement establishes state; correction converges through a
  bounded exponential filter rather than reacting fully to one scheduling delay.
- Rate correction is clamped to +/-1000 ppm. Invalid/overflowing observations fail
  visibly instead of producing an unbounded ratio.
- Video alignment is bounded per input callback. It cannot create an unbounded
  duplicate burst after a stall.
- Audio samples are never silently discarded. Small correction is implemented by
  the FFmpeg resampler; queue exhaustion remains the terminal R0-05 behavior.
- Every drop, duplicate, current drift, maximum drift, and rate correction is exposed
  in immutable diagnostics.
- Quarantine never follows a symlink/reparse point, never escapes the package, and
  never overwrites an existing file.

## ClockCoordinator

`cs_sync` is a new Qt-free module. `ClockCoordinator` is registered with the active
recording sources before the engine attaches sinks. It owns per-source observations
and a single master identity.

For the latest master observation `(masterTimestamp, masterObservedAt)`, a follower
sample received at `observedAt` compares itself with:

```text
estimated_master = masterTimestamp + (observedAt - masterObservedAt)
raw_drift        = sourceTimestamp - estimated_master
```

The interpolation removes ordinary spacing between independent callback queues while
remaining in the monotonic project-clock domain. A filtered offset corrects media PTS.
The slope of filtered drift over master elapsed time produces a bounded ppm estimate.

The coordinator returns:

- corrected project timestamp;
- filtered drift and maximum absolute drift;
- audio output/input sample ratio;
- whether this source is the master.

Observations are mutex-protected because native callbacks may arrive on different
queues. They perform no I/O and allocate no growing history.

## Video alignment

Each production video router owns a `VideoSyncPlanner` configured for its nominal
role cadence (screen 60 fps, camera/composite 30 fps). It consumes coordinator-
corrected timestamps and a retained last frame.

- A frame older than the next master-grid point by more than half a period is dropped.
- A gap newer than the grid by more than half a period duplicates the last frame.
- At most two duplicates are emitted for one incoming frame; a larger discontinuity
  jumps the grid forward and records the skipped interval rather than creating a burst.
- A passed frame is timestamped on the selected grid point, so the encoder receives a
  monotonic source aligned to the master.

The retained platform handle is shared, not copied. Drop and duplicate counters remain
separate from native capture drops and R0-05 queue displacement.

## Audio correction

`AudioBlock` carries a neutral `sampleRateRatio` with a default of 1.0. The production
audio router applies the coordinator's corrected timestamp and ratio. The master stream
always uses 1.0.

`FfmpegAudioSegmentEncoder` accumulates fractional sample correction and calls
libswresample compensation only when at least one whole sample is due. This avoids
rounding a 100 ppm correction to zero on every small block. Correction stays inside the
FFmpeg adapter; no FFmpeg type crosses the neutral media or recorder boundary.

## Interrupted `.part` quarantine

The database exposes the WRITING segment identities and relative final paths for one
recording session. The package store derives each R0-05 temporary path:

```text
.tmp/<relative-final-path>.part
```

and moves an existing regular file to:

```text
recovery/quarantine/<session-id>/<relative-final-path>.part
```

The destination directories are package-contained. Existing destination files are
never replaced. Missing source plus existing destination means a previous recovery
attempt already completed that move. Missing both is valid because a crash may happen
after the WRITING row but before file creation.

Filesystem quarantine happens before the SQLite recovery transaction. Therefore:

- crash after some moves: retry recognizes already moved files and continues;
- DB failure after moves: retry performs the DB transition without overwriting files;
- successful DB recovery cannot leave an unprocessed known `.part` behind.

Unreferenced `.part` files are quarantined separately during package recovery with a
stable orphan subtree, because they have no trustworthy row identity. Symlinks, reparse
points, hard-linked files, and paths escaping the package are rejected.

## Verification

Deterministic tests cover:

- master selection and monotonic interpolation;
- constant offset versus rate drift;
- ppm clamping and invalid observation rejection;
- video pass/drop/duplicate decisions and the per-callback duplicate cap;
- fractional audio correction accumulation in real FFmpeg output;
- two-hour accelerated clocks with final absolute drift below 40 ms;
- process death with one READY segment and one WRITING `.part`;
- no-overwrite quarantine, missing-source retry, orphan handling, and path attacks;
- recovered stop time at the last READY segment, limiting committed loss to one
  two-second segment.

## Honest acceptance boundary

Accelerated two-hour tests prove arithmetic, policies, and bounded state. They are not
evidence of a particular microphone oscillator, driver callback jitter, thermal state,
or disk throughput. The roadmap's physical two-hour A/V acceptance and forced-kill
exercise still require the intended macOS capture hardware after R0-03/R0-04 Apple
acceptance is available.
