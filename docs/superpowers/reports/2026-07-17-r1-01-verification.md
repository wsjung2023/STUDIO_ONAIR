# R1-01 Timeline Foundation Verification

Date: 2026-07-17

Scope: R1 delivery item 1 only. This report does not claim that the complete R1
recorder, editor, preview, or export workflow is finished.

## Delivered contracts

- Qt-free typed media assets and multi-track timeline aggregate.
- Split, trim, range delete/ripple, bounded Undo/Redo, and clean checkpoint.
- SQLite migration 002 with normalized snapshots, edit audit events, and
  checkpoints.
- Atomic durable edits and coherent snapshot/history reopen through one read
  transaction.
- Screen/camera/microphone acceptance workflow covering edit, undo/redo,
  explicit save, close, and exact reopen.
- Corruption guards for locale-dependent numbers, signed zero, integer
  overflow, duplicate generated IDs, invalid checkpoints, and out-of-range
  persisted values.
- Concurrent idempotent media-asset registration.

## Reproducible verification

Configuration: Windows x64, MSVC 2022, Qt 6.8.3, Ninja Debug, FFmpeg-disabled
test configuration.

```powershell
cmake --build build/r1-01-debug --clean-first
```

Result: exit code 0; 215 old outputs removed and all 223 build steps completed.
The build uses `/W4 /permissive- /WX` for project targets.

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
ctest --test-dir build/r1-01-debug --output-on-failure
```

Result: **421/421 tests passed**, 0 failed, 37.26 seconds.

The fresh run executed the newly linked Qt application tests successfully.
TimelineEditService tests intentionally run in `cs_tests.exe` because the
service is Qt-free; Qt application test discovery happens at test time. This
also avoids Windows App Control blocking a newly generated test executable
before CMake can finish test discovery. No policy, certificate store, or
security setting was weakened.

```powershell
rg -n '^[ \t]*#include' -- src/domain src/project_store |
    Select-String -Pattern 'Q|libav|mlt' -CaseSensitive
git diff --check
```

Result: no Qt, FFmpeg/libav, or MLT includes in the domain and project-store
layers; no whitespace errors.

Migration 002 introduces no third-party dependency, so `legal/OSS_BOM.csv`
does not change.

## Review hardening

The completion review found and the branch fixed:

- locale-sensitive floating-point JSON and signed-zero replay;
- non-coherent independent snapshot/history reads;
- unsigned JSON overflow and inconsistent command undo payloads;
- concurrent non-idempotent asset insertion;
- unchecked persisted int32/checkpoint ranges;
- edit-event sequence gaps and revision-count mismatches;
- valid-looking undo payloads that did not reconstruct the stored snapshot;
- package-path conflicts reported as a missing asset instead of an identity conflict;
- duplicate JSON string escaping and schema-fragile positional inserts.

## Platform boundary and next work

This verification was run on Windows. macOS compilation and supported-hardware
physical tests remain release gates and are not inferred from this result.

Next R1 slice: implement the edit-engine adapter contract and Editor view models
against a deterministic fake engine, then connect the durable timeline service
without moving product truth into MLT XML.
