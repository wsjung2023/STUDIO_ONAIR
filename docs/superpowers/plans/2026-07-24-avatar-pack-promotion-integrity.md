# Avatar Pack Promotion Integrity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make sealed avatar-pack promotion verify the exact staged tree and
report post-rename durability uncertainty without rollback, while eliminating
Windows cleanup handle leaks and POSIX per-directory descriptor retention.

**Architecture:** The sealed lease remains the source of expected topology,
identity, size, and SHA-256. Promotion re-enumerates the real tree immediately
before rename. Windows holds every verified file handle without write/delete
sharing through content verification, performs a final topology pass, and
closes child handles immediately before the root-handle rename because those
share modes otherwise prevent ancestor rename. POSIX traverses from the
retained root descriptor inside a trusted-private same-euid parent. Rename
consumes the lease, after which source and destination parent flush results map
to a value outcome rather than an error. Cleanup owns every transient Windows
handle with RAII.

**Tech Stack:** C++20, Win32 file handles and directory enumeration, POSIX
`openat`/`fdopendir`/`renameat2`, libsodium SHA-256, GoogleTest, CMake/Ninja,
MSVC AddressSanitizer.

## Global Constraints

- Pre-rename failures return an error, publish no destination, and leave the
  lease active for retry.
- Rename success consumes the lease immediately and is never rolled back.
- `PromotionOutcome::Durable` requires successful source- and
  destination-parent durability confirmation.
- Either durability failure returns `PromotionOutcome::Indeterminate`; it does
  not return an error.
- Exact promotion verification rejects missing, unknown, file/directory type,
  identity, size, or SHA-256 mismatches.
- Every expected Windows file is held without write/delete sharing through
  content verification, including at the 10,000-entry cap. A final topology
  pass precedes immediate close-and-rename inside a current-user-owned
  trusted-private parent. Same-user malicious processes are outside scope.
- POSIX construction retains only the staging parent and root descriptors.
- Allocations and exceptions inside public `noexcept` integrity paths are
  translated to `Result` errors.
- No public test hooks or path exposure are introduced.

---

### Task 1: Promotion outcome and durability state machine

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.h`
- Add: `src/avatar_pack_adapter/AvatarPackPromotionInternal.h`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

**Interfaces:**
- Add `enum class PromotionOutcome { Durable, Indeterminate };`.
- Change `promoteTo(...) && noexcept` to
  `core::Result<PromotionOutcome>`.

- [x] **Step 1: Add RED compile and state-machine tests**

Assert the exact rvalue return type expected by Task 6. Exercise an internal,
non-public durability helper with all four source/destination flush outcomes,
assert both callbacks are always attempted, and assert any failure maps to
`Indeterminate`.

- [x] **Step 2: Prove RED**

Build the focused test target. Expected: the old `Result<void>` contract and
missing state helper fail compilation.

- [x] **Step 3: Implement post-rename value semantics**

Consume the lease as soon as no-replace rename succeeds. Attempt both parent
flushes independently, remove rollback, close retained authority, and return
`Durable` or `Indeterminate`. Preserve ordinary pre-rename errors and retry.

- [x] **Step 4: Prove GREEN**

Run the focused outcome, success, conflict, unsafe-parent, and retry tests.

---

### Task 2: Exact promotion integrity and Windows file leases

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

- [x] **Step 1: Add RED integrity regressions**

After sealing, test same-identity in-place mutation, an unknown file, an
unknown directory, and a concurrent writable handle. Every attempt must return
an error, leave the destination absent, and allow a repaired lease to retry.
Extend the maximum-shape Windows fixture through successful promotion.

- [x] **Step 2: Prove RED**

Run each new test against identity-only promotion. Expected: mutation/topology
cases wrongly promote or the concurrent handle is not excluded.

- [x] **Step 3: Implement exact enumeration and streaming hashes**

Build expected topology from sealed directory/file records. Enumerate the
actual tree, reject unknown/missing/type/identity differences, stream every
file through size and SHA-256 verification, and flush verified files. On
Windows retain RAII handles opened without write/delete sharing through
content verification, perform a final topology pass, then close them
immediately before root rename. The current-user-owned trusted-private parent
is the exclusion boundary for that final close-and-rename step. Translate
allocation and hashing exceptions to stable `Result` errors.

- [x] **Step 4: Prove GREEN**

Run all integrity regressions, existing reparse/conflict coverage, and the
10,000-entry promotion fixture.

---

### Task 3: Windows cleanup RAII and failure paths

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

- [x] **Step 1: Add RED leak regressions**

Measure process handle count across a known-file identity mismatch and a real
`SetFileInformationByHandle(FileDispositionInfo)` failure. Assert exact handle
count stability and preservation of the replacement or protected content.

- [x] **Step 2: Prove RED**

Run both regressions. Expected: the short-circuit file cleanup path leaks the
locally opened handle.

- [x] **Step 3: Make local ownership unconditional**

Introduce small Win32 handle scope guards. Compute identity match, disposition,
and close results independently, and evaluate the aggregate only after close.
Use the same ownership rule for transient directory and enumeration handles.

- [x] **Step 4: Prove GREEN**

Run leak, replacement, blocked-cleanup, junction, and repeated race tests.

---

### Task 4: POSIX root-descriptor traversal

**Files:**
- Modify: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/AvatarPackStaging.cpp`

- [x] **Step 1: Add the maximum unique-directory contract**

Add a POSIX-only maximum-shape fixture with unique subdirectories and bounded
descriptor-count assertions. Keep the existing Windows maximum-shape handle
coverage.

- [x] **Step 2: Prove RED**

On Linux, construction retains one descriptor per directory and exceeds the
bounded descriptor expectation.

- [x] **Step 3: Reopen traversal from root**

Store directory identities and relative components, not child descriptors.
For create, seal, read, cleanup, and promotion, reopen each component from the
root with `openat`/`O_NOFOLLOW`, verify stored identities, and close transient
descriptors per operation. Enumerate and hash promotion content from rootfd.

- [x] **Step 4: Prove GREEN where executable**

Compile the POSIX translation unit with GCC. Run the POSIX focused fixture when
an audited Linux runtime is available; otherwise record the concrete runtime
limitation and the compile evidence.

---

### Task 5: Verification, documentation, and commit

**Files:**
- Modify: `docs/superpowers/specs/2026-07-24-avatar-pack-capabilities-design.md`
- Modify: `.superpowers/sdd/foundation-task-5-report.md` (ignored)
- Modify: this plan when implementation differs.

- [x] **Step 1: Focused and repeat gates**

Run outcome, integrity, cleanup, maximum-shape, and promotion tests repeatedly,
then run the complete avatar-pack debug suite.

- [x] **Step 2: Sanitizer and POSIX gates**

Run the complete avatar-pack MSVC ASan suite and compile the POSIX translation
unit with GCC. Run an actual POSIX focused test if the audited runtime permits.

- [x] **Step 3: Full build and CTest**

Run the enabled build and the complete CTest matrix. Confirm the Task 6 compile
smoke uses `Result<PromotionOutcome>`.

- [x] **Step 4: Review and commit**

Update the evidence report, inspect scoped diffs, verify no unrelated files,
and create one separate fix commit:

```text
fix(avatar): verify promotion integrity before commit
```
