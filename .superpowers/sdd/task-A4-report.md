# Task A4 Report — AvatarMotionNdjsonSink

## What was built

- `src/avatar/AvatarMotionNdjsonSink.h` / `.cpp`: `class creator::avatar::AvatarMotionNdjsonSink`.
  - Constructor: `explicit AvatarMotionNdjsonSink(std::filesystem::path directory)` — no I/O, just
    stores the directory (normally the resolved `<pkg>/telemetry/` dir the project store creates).
  - `[[nodiscard]] core::Result<void> append(const AvatarMotionSample&)` — serializes via
    `AvatarMotionSerializer::toNdjsonLine` and appends the line to
    `<directory>/avatar-motion.ndjson` (`kFileName` is a public `static constexpr const char*`).
- `src/avatar/CMakeLists.txt`: added the two new sources to `cs_avatar`.
- `tests/CMakeLists.txt`: added `avatar/AvatarMotionNdjsonSinkTest.cpp` to `cs_tests`.
- `tests/avatar/AvatarMotionNdjsonSinkTest.cpp`: 7 new test cases (below).

## Durable append vs. atomic rename (why not reuse `writeFileDurably`)

`project_store::internal::writeFileDurably` (used for the manifest) writes to a temp file next to
the target, flushes/fsyncs, then atomically renames over the target — correct for a file that is
*replaced wholesale* on every save. Telemetry is structurally different: ARCHITECTURE.md §7.4 says
"대량 이벤트는 NDJSON으로 순차 기록한다" (high-volume events are recorded sequentially as NDJSON) — every
sample only ever *extends* a file that keeps growing for the life of a recording. Atomic-rename-per-
append would mean rewriting the entire accumulated history to a temp file on every single call,
which is wasteful and doesn't even model the right operation (there is no "replace the whole file"
semantic here, only "add one line").

So `append()` instead:
1. builds `directory_ /= kFileName` (path object, not string concatenation — see below),
2. opens `std::ofstream` in `std::ios::binary | std::ios::app` mode,
3. checks `is_open()` before writing (covers "parent directory missing" and "directory is actually
   a file" without needing an exception),
4. writes the line, calls `flush()`, and checks the stream's fail state after,
5. relies on RAII (the `ofstream` destructor) to close the file on every return path.

This is stream-level durability (data leaves process memory and reaches the OS) — it does not call
`fsync`/`FlushFileBuffers` the way `writeFileDurably` does for the manifest. That's a deliberate,
narrower guarantee matching the task brief's literal wording ("append + flush; NOT whole-file
atomic-rename") for a path that may be called once per tracked frame; per-line `fsync` would impose
a much heavier cost on a high-frequency telemetry path. If a stronger guarantee is ever needed here,
it's a follow-up, not something this task silently assumed.

## Closing the A3 handoff: tNs >= 0 enforcement

A3's `AvatarMotionSerializer` documents that it will **not** clamp a negative timestamp — clamping
would hide a producer bug (CLAUDE.md 9), so it lets the resulting JSON fail schema validation
instead, deferring runtime enforcement to whoever writes the file. This sink is that boundary:

```cpp
if (sample.timestamp.time_since_epoch().count() < 0) {
    return AppError{ErrorCode::InvalidArgument,
                    "avatar.motion sample has a negative timestamp; "
                    "the event schema requires tNs >= 0"};
}
```

This check runs **before** any I/O — a rejected sample creates no file (if none existed) and never
extends an existing one. This is a targeted check, not full per-line schema validation, because the
serializer's output shape is fixed (nine numeric fields + type + provider), so `tNs >= 0` is the only
data-dependent constraint the schema actually imposes on this write path.

Verified with a RED/GREEN toggle (see below): the check was temporarily disabled and both negative-
timestamp tests failed as expected, then re-enabled and the suite went green — proving the tests
are not vacuous.

## Non-ASCII path handling

`append()` builds the on-disk path with `std::filesystem::path::operator/=` on the path object
(`filePath /= kFileName`), never `directory.string() + "/avatar-motion.ndjson"`. On Windows,
`path::string()` narrows through the process ANSI codepage (949 on this machine) and is lossy for
non-ASCII components — the exact bug that shipped in R0-01 Task 9. Test
`NonAsciiTelemetryDirectoryRoundTrips` creates a directory named `u8"텔레메트리_日本語_🎬"` (Hangul +
Japanese + emoji, built via the `char8_t` `fs::path` constructor overload — portable by C++20
standard, not by platform coincidence, per the same reasoning `JsonProjectStoreTest` documents),
appends a sample, and reads the line back, asserting it's present and schema-valid.

## TDD RED/GREEN

The header/impl/test were authored together, then verified with an explicit RED pass rather than
trusting the GREEN result alone: I disabled the `tNs >= 0` guard (commented it out, replaced with
`(void)sample;`), rebuilt, and reran the two negative-timestamp tests — both failed exactly as
expected:

```
[  FAILED  ] AvatarMotionNdjsonSinkTest.NegativeTimestampRejectedAndWritesNothing
[  FAILED  ] AvatarMotionNdjsonSinkTest.NegativeTimestampAfterValidAppendsDoesNotExtendTheFile
71% tests passed, 2 tests failed out of 7
```

Then restored the guard, rebuilt, reran: all 7 sink tests green, and the full `cs_tests` suite came
back at 319/319 real tests passing (312 baseline + 7 new), with only the 2 expected
`cs_app_tests_NOT_BUILT` / `cs_qml_tests_NOT_BUILT` guard "failures" (not run, by design, when only
`cs_tests` is built).

## Error-path / no-exception-escapes evidence

- `MissingParentDirectoryReturnsIoFailureWithNoThrow`: constructs the sink over
  `<tmp>/does/not/exist` (no such directory tree); `append()` returns
  `AppError{ErrorCode::IoFailure}` and the whole call is wrapped in `EXPECT_NO_THROW`. Passed.
- `DirectoryThatIsActuallyAFileReturnsIoFailureWithNoThrow`: creates a regular file at the path
  the sink is told is its directory, then calls `append()`; same assertion. Passed.
- Both cases are caught structurally: `std::ofstream` doesn't throw with the default exception mask
  (we never call `.exceptions(...)`), so `is_open()` being false is what drives the `IoFailure`
  return — the `try`/`catch` around the path-building + stream block is defensive (covers
  `std::filesystem::path` construction throwing `std::system_error` on an unrepresentable path, and
  any stream exception a future change might introduce), and both blocks catch `std::exception` and
  translate to `AppError`, so nothing crosses the `append()` boundary uncaught.

## Files changed

- `src/avatar/AvatarMotionNdjsonSink.h` (new)
- `src/avatar/AvatarMotionNdjsonSink.cpp` (new)
- `src/avatar/CMakeLists.txt` (added 2 sources)
- `tests/CMakeLists.txt` (added 1 test source)
- `tests/avatar/AvatarMotionNdjsonSinkTest.cpp` (new, 7 tests)

## Self-review

- Path building audited: the only `directory_`-derived path in `append()` is built with `/=`, never
  string concatenation. Error *messages* elsewhere in the codebase (e.g. `JsonProjectStore.cpp`) do
  use `.string()` for human-readable text, which is a different (and acceptable) use than building
  the actual I/O path — I did not need `.string()` anywhere in this sink at all.
- `cs_avatar` still builds with no new dependencies beyond what A1–A3 already added
  (`cs_core`, `cs_media`, `cs_domain`, `nlohmann_json`) — no Qt, no OSS_BOM change needed.
- Confirmed `/W4 /permissive- /WX` clean build (no warnings) for both the library and the new test.
- Confirmed the fixture's `TearDown()` always runs (it's a normal gtest `TEST_F`, not an early
  `return`/exception path) even when a test body's assertion fails — matches
  `JsonProjectStoreTest`'s established pattern, which the same worktree already relies on elsewhere.
- Did not add an `fsync`/`FlushFileBuffers` call. Documented why above; flagging as a concern below
  in case the plan intended stronger durability than the brief's literal wording suggests.

## Concerns

- **Durability strength**: `append()` provides "flush to OS" durability (via `std::ofstream::flush()`),
  not "fsync to disk" durability (unlike the manifest's `writeFileDurably`, which calls
  `FlushFileBuffers`/`fsync` and even fsyncs the parent directory on POSIX). The task brief's own
  wording ("append + flush") matches what was built, but if the product actually needs crash-safe
  telemetry (surviving an OS crash, not just a process crash), a future task should add an
  fsync-equivalent here. Flagging rather than silently deciding this doesn't matter.
- Per-sample `tNs >= 0` is the only schema constraint enforced at this boundary, per the brief's
  explicit instruction that "full per-line schema validation is not required on this write path".
  If a future task adds new fields to `ExpressionParameters` that could produce non-numeric values,
  this narrow check would not catch it — flagging so it isn't forgotten.
