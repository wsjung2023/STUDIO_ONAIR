# Avatar Pack Capability Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate one immutable archive byte source and return staged avatar
packs as retained, move-only filesystem capabilities.

**Architecture:** Windows retains one no-follow, write/delete-denying archive
handle through raw preflight and miniz; POSIX copies one no-follow source
descriptor into an unlinked private snapshot. `AvatarPackStaging` becomes the
sealed lease held by `ValidatedAvatarPack`, and all file consumption is
handle-relative with root identity checked before cleanup.

**Tech Stack:** C++20, Win32 file handles, POSIX `openat`/`unlinkat`, miniz,
libsodium, GoogleTest, CMake/Ninja, MSVC AddressSanitizer.

## Global Constraints

- Do not reopen the archive path after the initial no-follow open.
- Windows must avoid a whole-archive copy; POSIX must not trust advisory locks.
- A path is never authority for staged-file reads or cleanup.
- No production test hooks, fake filesystems, or path-based fallback.
- Every public allocation/filesystem/JSON boundary returns `core::Result`.
- Cleanup identity failure must not delete a replacement.

---

### Task 1: Immutable archive source and EOCD selection

**Files:**
- Modify: `src/avatar_pack_adapter/AvatarPackArchive.h`
- Modify: `src/avatar_pack_adapter/AvatarPackArchive.cpp`
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`

**Interfaces:**
- `AvatarPackArchive::open(const std::filesystem::path&) noexcept`
- One owned `FILE*` shared by `preflightRawZip` and
  `mz_zip_reader_init_cfile`.

- [x] **Step 1: Write failing real-filesystem tests**

Add Windows fixtures that pre-open the package for writing, then assert
validation fails without staging; after validation reaches staging, attempt
central/EOCD mutation and `MoveFileExW`, assert all operations fail and the
original pack validates. Add an archive containing a true EOCD plus a second
structurally valid zero-entry EOCD in its comment and expect
`avatar.pack.archive.envelope`.

- [x] **Step 2: Verify RED**

Run:

```powershell
.\build\windows-avatar-packs-debug\cs_avatar_pack_tests.exe `
  --gtest_filter='*ImmutableArchive*:*MultipleStructurallyValidEocd*'
```

Expected: source mutation/replacement succeeds under `_SH_DENYNO`, and the
false EOCD candidate is selected instead of producing the envelope error.

- [x] **Step 3: Implement handle-first input**

Use this platform split:

```cpp
// Windows
CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
GetFileInformationByHandle(handle, &info);
GetFileSizeEx(handle, &size);
_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
_fdopen(descriptor, "rb");

// POSIX
open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
fstat(source, &info);
mkstemp(privateTemplate);
fchmod(snapshot, 0600);
unlink(privateTemplate);
copy at most kMaximumArchiveBytes + 1 bytes;
fdopen(snapshot, "rb");
```

Split candidate validation from tail discovery. Evaluate every end-matching
EOCD candidate and accept only when one complete candidate succeeds.

- [x] **Step 4: Verify GREEN**

Run the Task 1 filter and the existing seven-test envelope gate. Expected:
all pass.

---

### Task 2: Sealed staging lease

**Files:**
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.h`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackValidator.h`
- Modify: `src/avatar_pack_adapter/AvatarPackValidator.cpp`
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`

**Interfaces:**
- `ValidatedAvatarPack { AvatarAssetManifest manifest; AvatarPackStaging staging; }`
- `displayPath() const` is diagnostic only.
- `exists(relativePath) const noexcept`
- `read(relativePath, maximumBytes) const noexcept`
- `cleanup() noexcept`
- private friend-only `create`, `writeNewFile`, `extractNewFile`, and `seal`.

- [x] **Step 1: Write capability API and rename/replacement tests**

Convert successful-fixture assertions to:

```cpp
auto bytes = result.value().staging.read("payload/model.bin", 1024U);
ASSERT_TRUE(bytes.hasValue());
EXPECT_EQ(bytes.value(), expected);
```

Add a test that tries to rename the sealed root and creates a replacement when
the platform permits it. Assert lease reads return the original bytes or fail
closed, never replacement bytes. Call `cleanup()` and assert a renamed or
replaced root yields `avatar.pack.staging.cleanup` while replacement remains.

- [x] **Step 2: Verify RED**

Expected: `ValidatedAvatarPack` has only `stagingRoot`, retained handles were
closed by `finish`, and reads follow the replacement path.

- [x] **Step 3: Implement sealed capability**

Keep the root and directory handles/descriptors open after `seal`. Implement
Windows reads by `CreateFileW` under the retained verified final root followed
by exact final-path, regular-file, and size checks. Implement POSIX reads with
component-by-component `openat(..., O_NOFOLLOW)` from the retained root.

Before deletion, compare the current parent/name file identity with the
retained root:

```cpp
// Windows: volume serial + FileIndexHigh/FileIndexLow
// POSIX: st_dev + st_ino from fstatat(..., AT_SYMLINK_NOFOLLOW)
```

On mismatch, close owned handles, leave both trees untouched, and return the
stable cleanup error.

- [x] **Step 4: Verify GREEN**

Run capability/rename tests three times and the complete validator binary.

---

### Task 3: Result-only exception boundaries

**Files:**
- Modify: `src/avatar_pack_adapter/AvatarPackArchive.h`
- Modify: `src/avatar_pack_adapter/AvatarPackArchive.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.h`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackValidator.h`
- Modify: `src/avatar_pack_adapter/AvatarPackValidator.cpp`
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`

**Interfaces:**
- `validateAndExtract(...) const noexcept`
- archive `open/read/stream` and lease `exists/read/cleanup` are `noexcept`.

- [x] **Step 1: Write contract tests**

Add compile-time assertions:

```cpp
static_assert(noexcept(std::declval<const AvatarPackValidator&>()
                           .validateAndExtract(
                               std::declval<const fs::path&>())));
static_assert(!std::is_copy_constructible_v<AvatarPackStaging>);
static_assert(std::is_move_constructible_v<AvatarPackStaging>);
```

Run invalid paths, malformed bounded JSON, and malformed archives through
`EXPECT_NO_THROW`; each must return an error.

- [x] **Step 2: Verify RED**

Expected: the `noexcept` assertion fails and staging is not publicly owned by
the result.

- [x] **Step 3: Implement nested exception translation**

Catch `std::bad_alloc` as `InsufficientStorage`; catch filesystem, JSON, and
other standard exceptions as stable `IoFailure`; catch unknown exceptions.
After staging exists, route every catch through explicit cleanup and prefer
cleanup failure. Sort manifest payload pointers rather than copying payload
records.

- [x] **Step 4: Verify GREEN**

Run the exception-contract filter and all avatar-pack tests.

---

### Task 4: Final verification and handoff

**Files:**
- Modify: `.superpowers/sdd/foundation-task-5-report.md` (ignored handoff)
- Modify: this plan only if implementation details materially differ.

- [x] **Step 1: Focused repetition**

Run immutable archive, EOCD, capability, cleanup, exception, and fuzz tests
three times with `--gtest_break_on_failure`.

- [x] **Step 2: MSVC AddressSanitizer**

Build and run `cs_avatar_pack_tests` in
`build/windows-avatar-packs-asan`; require no diagnostics.

- [x] **Step 3: Enabled full verification**

Build `build/windows-avatar-packs-debug`, then run:

```powershell
ctest --test-dir build/windows-avatar-packs-debug `
  --output-on-failure --no-tests=error
```

- [x] **Step 4: Report and commit**

Record RED/GREEN evidence, platform trade-offs, sealed-capability deviation
from the original bare-path brief, exact counts, and commit hash. Run
`git diff --check`, stage only scoped files, commit with:

```text
fix(avatar): retain verified pack capabilities
```

Verify tracked status is clean.
