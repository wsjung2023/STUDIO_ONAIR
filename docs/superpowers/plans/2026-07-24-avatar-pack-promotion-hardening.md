# Avatar Pack Promotion Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind raw ZIP approval to the EOCD miniz will select, make cleanup
race-safe within documented platform trust boundaries, and give Task 6 a
path-free atomic promotion capability without per-entry retained handles.

**Architecture:** Accept exactly one EOCD signature in the searchable immutable
tail. Keep only staging root/parent authority after sealing, reopen entries
no-follow for identity/hash verification, delete Windows objects through
verified delete handles, require trusted-private POSIX parents, and promote the
retained root with platform no-replace rename primitives.

**Tech Stack:** C++20, Win32 `SetFileInformationByHandle`, POSIX
`openat`/`renameat2`, miniz, libsodium, GoogleTest, CMake/Ninja, MSVC ASan.

## Global Constraints

- No public filesystem path or path-like diagnostic for sealed staging.
- Task 6 consumes `AvatarPackStaging&&` and calls `promoteTo(finalPath) &&`.
- Failed cleanup never deletes a replacement object.
- Failed promotion leaves the capability active and creates no partial target.
- Sealing retains no file handle and no non-root directory handle.
- Public reads verify stored identity, byte count, and SHA-256 after no-follow
  reopen.
- POSIX source and destination parents must be owned by the effective user and
  have no group/other permission bits.
- Every regression uses real temporary files; no production test hooks.

---

### Task 1: Exact miniz EOCD parity

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackArchive.cpp`

**Interfaces:**
- Existing `AvatarPackArchive::open(path) noexcept`.

- [x] **Step 1: Add a fake-last-EOCD regression**

Create a valid signed ZIP whose true EOCD comment contains a later
`PK\x05\x06` record with an oversized central-directory size. Assert
`avatar.pack.archive.envelope` and an empty staging parent.

- [x] **Step 2: Prove RED**

Run the single regression. Expected: validation reaches miniz or returns a
non-envelope error because raw preflight approves the earlier true EOCD.

- [x] **Step 3: Enforce one searchable EOCD signature**

Count every EOCD signature in the bounded tail. Require exactly one, require
its declared comment to end at the immutable archive boundary, and run the
complete structural/cap validation on that exact candidate.

- [x] **Step 4: Prove GREEN**

Run the fake-last and existing multiple-EOCD tests. Expected: both pass.

---

### Task 2: Path-free sealed capability and bounded handles

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.h`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

**Interfaces:**
- Keep `exists(relativePath) const noexcept`.
- Keep `read(relativePath, maximumBytes) const noexcept`.
- Remove `displayPath()`.

- [x] **Step 1: Add compile and large-shape regressions**

Add a `HasDisplayPath` concept and `static_assert(!HasDisplayPath<...>)`.
Build a signed fixture with 10,000 total entries and distinct small payload
contents (the manifest requires unique payload hashes), then validate it
without descriptor exhaustion.

- [x] **Step 2: Prove RED**

Expected: the compile contract fails because `displayPath()` exists; current
sealing also attempts to retain one handle per entry.

- [x] **Step 3: Reopen and verify entries**

At seal, compute/store each file byte count and SHA-256, then close every file
and non-root directory handle/descriptor. For `exists` and `read`, reopen from
the retained root with no-follow semantics and verify identity, size, and hash.

- [x] **Step 4: Prove GREEN**

Run the compile, normal read, replacement, and 10,000-entry tests.

---

### Task 3: Identity-safe cleanup

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

**Interfaces:**
- `cleanup() noexcept` returns `avatar.pack.staging.cleanup` on uncertainty.

- [x] **Step 1: Add replacement-preservation repetition**

Discover the opaque staging child only from the test-owned temporary parent,
attempt rename/replacement concurrently with cleanup for repeated iterations,
and assert every created replacement survives whenever the attack succeeds.

- [x] **Step 2: Prove RED**

Expected: Windows cleanup still uses `DeleteFileW`/`RemoveDirectoryW` after a
separate identity check.

- [x] **Step 3: Implement platform cleanup**

Open Windows roots/directories with `DELETE`; reopen each stored file and child
directory with delete authority, verify identity, and call
`SetFileInformationByHandle(FileDispositionInfo)` deepest-first. On POSIX,
reject an untrusted staging parent at creation, verify parent/root identities,
unlink children relative to verified descriptors, and refuse the top-level
unlink on any mismatch.

- [x] **Step 4: Prove GREEN**

Run cleanup failure, replacement, junction, and repeated race tests.

---

### Task 4: Atomic no-replace promotion

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.h`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

**Interfaces:**
- Add `core::Result<void> promoteTo(const std::filesystem::path&) && noexcept`.

- [x] **Step 1: Add promotion contract regressions**

Cover successful promotion/readback, `AlreadyExists`, reparse or untrusted
destination parent, and a failed promotion followed by successful retry.
Assert the lvalue overload is unavailable and the rvalue call is `noexcept`.

- [x] **Step 2: Prove RED**

Expected: compilation fails because `promoteTo` does not exist.

- [x] **Step 3: Implement promotion**

Verify the complete staged tree and flush every file. Open and validate the
destination parent no-follow. Rename the retained Windows root handle with
`FILE_RENAME_INFO` and `ReplaceIfExists=FALSE`; on Linux use
`renameat2(..., RENAME_NOREPLACE)`, and fail closed where no atomic exclusive
rename exists. Flush the destination parent and disarm cleanup only after the
rename and durability step succeed.

- [x] **Step 4: Prove GREEN and Task 6 compatibility**

Compile a minimal Task 6-shaped call that moves
`validated.value().staging` into `promoteTo(finalVersionPath)`. Run all
promotion tests.

---

### Task 5: Verification and handoff

**Files:**
- Modify: `.superpowers/sdd/foundation-task-5-report.md` (ignored)
- Modify: capability design and this plan if implementation differs.

- [x] **Step 1: Focused repeat and full avatar binary**

Run EOCD, cleanup-race, capability, 10,000-entry, and promotion tests three
times, then run the complete avatar-pack binary.

- [x] **Step 2: MSVC AddressSanitizer**

Rebuild and run the complete avatar-pack binary under ASan with no diagnostics.

- [x] **Step 3: Full build and CTest**

Run dependency audit, enabled build, and complete CTest with zero failures.

- [x] **Step 4: Report and commit**

Record RED/GREEN evidence, POSIX same-euid trust limitation, and the unavoidable
`AppError` allocation caveat for `noexcept` boundaries. Stage only scoped
files, run both diff checks, and commit:

```text
fix(avatar): harden pack promotion authority
```
