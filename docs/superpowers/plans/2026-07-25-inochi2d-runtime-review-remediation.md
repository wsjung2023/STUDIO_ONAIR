# Inochi2D Runtime Review Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the four independent-review findings so the pinned Inochi2D
runtime is complete, inspected as a real shared library, loaded without a
verification race, and staged only through contained filesystem operations.

**Architecture:** Keep the audited upstream source unchanged. Stage the exact
LDC 1.40 shared druntime and Phobos DLLs required by the built Windows runtime,
inspect PE files without executing them, retain deny-write/delete file leases,
and let the model runtime consume only a verified-and-loaded runtime object.
Unsupported binary formats fail closed until an equally complete parser and
lease strategy exists.

**Tech Stack:** C++20, PowerShell 5.1/7, PE32+ parsing, Win32 file and loader
APIs, CMake/CTest, GoogleTest, LDC/DUB.

## Global Constraints

- Preserve source commit/archive hashes, dependency hashes, exact LDC 1.40.0,
  build-local DUB cache, and `--skip-registry=all`.
- Do not execute an unverified runtime to discover its type, imports, or
  exports.
- Commit each review finding separately.
- Do not commit runtime or compiler binaries.

---

### Task 1: Complete and loadable Windows runtime closure

**Files:**
- Modify: `scripts/bootstrap_inochi2d.ps1`
- Modify: `scripts/verify_inochi2d_runtime.ps1`
- Modify: `tests/scripts/Inochi2dBootstrapPolicyTest.ps1`
- Modify: `legal/OSS_BOM.csv`

**Interfaces:**
- Produces: exact staged DLL closure and LDC license notice metadata.
- Consumes: official LDC 1.40.0 shared runtime files from `-LdcRoot`.

- [ ] Add policy tests requiring the two exact DLL hashes, LDC license hash,
  staged dependency manifest entries, import allowlist, and clean-load probe.
- [ ] Run the policy test and prove RED against commit `bb541fd`.
- [ ] Stage `druntime-ldc-shared.dll` and `phobos2-ldc-shared.dll`, append the
  exact LDC license bytes, record hashes, and reject any undeclared non-system
  import.
- [ ] Load from a clean copied prefix with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` and resolve
  all 16 symbols.
- [ ] Run policy/bootstrap/verifier and commit:
  `fix(avatar): stage complete Inochi2D runtime closure`.

### Task 2: Parse real shared-library structure and exports

**Files:**
- Create: `src/avatar/inochi2d/Inochi2dBinaryInspector.h`
- Create: `src/avatar/inochi2d/Inochi2dBinaryInspector.cpp`
- Modify: `src/avatar/inochi2d/Inochi2dRuntimeManifest.cpp`
- Modify: `scripts/verify_inochi2d_runtime.ps1`
- Create: `tests/fixtures/Inochi2dPeFixture.cpp`
- Modify: `tests/avatar/inochi2d/Inochi2dRuntimeManifestTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/avatar/CMakeLists.txt`

**Interfaces:**
- Produces: `inspectInochi2dBinary(path)` with type, architecture, imports, and
  actual exports.
- Consumes: immutable bytes only; never calls the loader.

- [ ] Build a real PE fixture DLL exporting the 16 names and add tests that
  reject a cleared DLL characteristic and a removed export.
- [ ] Run the focused tests and prove the current 128-byte-header fixture is
  rejected or the new API is absent.
- [ ] Implement bounds-checked PE32+ header, section/RVA, import-directory, and
  export-directory parsing. Return UnsupportedVersion for Mach-O/ELF until
  their full symbol parsers exist.
- [ ] Make both PowerShell and C++ verifiers compare actual exports and imports
  to policy and add an opt-in test against the actual bootstrapped DLL.
- [ ] Run focused/default/opt-in tests and commit:
  `fix(avatar): inspect actual Inochi2D binary exports`.

### Task 3: Retain verification authority through load

**Files:**
- Modify: `src/avatar/inochi2d/Inochi2dRuntimeManifest.h`
- Modify: `src/avatar/inochi2d/Inochi2dRuntimeManifest.cpp`
- Modify: `src/avatar/inochi2d/Inochi2dModelRuntime.h`
- Modify: `src/avatar/inochi2d/Inochi2dModelRuntime.cpp`
- Modify: `src/avatar/inochi2d/Inochi2dAvatarRenderer.h`
- Modify: `src/avatar/inochi2d/Inochi2dAvatarRenderer.cpp`
- Modify: `tests/avatar/inochi2d/Inochi2dRuntimeManifestTest.cpp`
- Modify: `tests/avatar/inochi2d/Inochi2dModelRuntimeTest.cpp`

**Interfaces:**
- Produces: move-only `Inochi2dVerifiedRuntime`, which owns root/artifact file
  leases and the hardened loaded module.
- Consumes: runtime root, not a caller-supplied raw library path.

- [ ] Add RED tests for an original-root junction and replacement/write/delete
  attempts while a verified runtime is retained.
- [ ] Reject the original root reparse point before canonicalization and open
  root/artifacts with Win32 handles that deny write/delete sharing.
- [ ] Offline-verify while leases are held, then hardened-load that same staged
  closure and resolve all 16 symbols before returning.
- [ ] Change model/renderer open paths to consume the verified runtime root;
  keep `loadAndVerify` explicitly diagnostic-only.
- [ ] Run focused/ASan tests and commit:
  `fix(avatar): retain runtime verification lease through load`.

### Task 4: Contain destructive and extraction operations

**Files:**
- Modify: `scripts/bootstrap_inochi2d.ps1`
- Modify: `tests/scripts/Inochi2dBootstrapPolicyTest.ps1`

**Interfaces:**
- Produces: OS-correct `Assert-StrictChildPath` and
  `Assert-NoReparseAncestors`.
- Consumes: every bootstrap deletion, extraction, and staging destination.

- [ ] Add structural RED assertions and real Windows junction rejection tests
  before compiler discovery.
- [ ] Select OrdinalIgnoreCase only on Windows and Ordinal on POSIX.
- [ ] Reject reparse ancestors and revalidate containment immediately before
  every recursive delete, archive extraction, and copy.
- [ ] Enumerate fresh extraction trees and reject links before reading or
  copying their contents.
- [ ] Run Windows policy tests plus a PowerShell-on-Linux case-sensitivity
  probe and GCC 14 warning gate; commit:
  `fix(build): contain Inochi2D bootstrap filesystem writes`.

### Task 5: Final verification and handoff

**Files:**
- Modify: `.superpowers/sdd/2d-task-1-report.md` (ignored evidence ledger)

**Interfaces:**
- Produces: exact current evidence for independent rereview.

- [ ] Rebootstrap the actual Windows runtime.
- [ ] Verify import closure, real DLL/export parsing, clean hardened load,
  post-verification mutation denial, and exact file set.
- [ ] Run policy, focused, default, opt-in, ASan, GCC 14, PowerShell AST/JSON,
  full CTest, and `git diff --check`.
- [ ] Record platform limitations without claiming unexecuted macOS/Android
  verification and request independent rereview.
