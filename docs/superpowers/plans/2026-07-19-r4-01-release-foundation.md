# R4-01 Cross-Platform Release Foundation Implementation Plan

> **Execution:** Follow `superpowers:executing-plans` and implement on
> `feat/r4-01-release-foundation`. The worktree is
> `.worktrees/r4-01-release-foundation`.

## Scope

Build the shared, Qt-free release-contract layer that lets the desktop and
future Android applications report supported features honestly and creates a
verified, atomic release manifest for final artifacts. This package does not
claim a signed installer, a store upload, or device validation.

## Task 1: Platform capability contract

**Files**

- Create: `src/platform_release/CMakeLists.txt`
- Create: `src/platform_release/PlatformCapability.h`
- Create: `src/platform_release/PlatformCapability.cpp`
- Create: `src/platform_release/PlatformCapabilityRegistry.h`
- Create: `src/platform_release/PlatformCapabilityRegistry.cpp`
- Create: `tests/platform_release/PlatformCapabilityTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

1. Write tests that reject empty/duplicate capability ids and unsupported state
   without an explanation; test immutable lookup and known desktop defaults.
2. Run the new test binary and observe its build/test failure before adding the
   implementation.
3. Implement value validation and a registry whose platform defaults are
   compile-time selected, with explicit unavailable reasons for unsupported
   features.
4. Add the Qt-free module and test target to CMake.
5. Run the capability test binary and the default suite.

## Task 2: Deterministic release manifest and durable writer

**Files**

- Create: `src/platform_release/ReleaseManifest.h`
- Create: `src/platform_release/ReleaseManifest.cpp`
- Create: `src/platform_release/ReleaseManifestStore.h`
- Create: `src/platform_release/ReleaseManifestStore.cpp`
- Create: `tests/platform_release/ReleaseManifestTest.cpp`
- Modify: `src/platform_release/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

1. Write tests for manifest validation, canonical ordering, SHA-256 verification,
   missing artifact rejection, and failure without a partial manifest.
2. Run the test binary and observe the expected compile/test failure.
3. Implement a manifest containing product version, source revision, target,
   artifact relative paths and SHA-256 hashes; validate names and hashes.
4. Implement a sibling temporary-file writer with atomic replace and a reader
   that revalidates every artifact hash before it accepts a manifest.
5. Run focused and default tests.

## Task 3: Release metadata target and CI artifact evidence

**Files**

- Create: `cmake/ReleaseManifest.json.in`
- Create: `scripts/write_release_manifest.ps1`
- Create: `tests/scripts/ReleaseManifestScriptTest.ps1`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/CMakeLists.txt`

1. Write a PowerShell test that makes a temporary artifact, writes metadata,
   validates its SHA-256, and rejects a changed artifact.
2. Run the script test and observe it fail before the script exists.
3. Add a release-manifest script with explicit product/revision/target inputs;
   it emits no credentials and cannot publish anything.
4. Add a `release-metadata` CMake target and CI artifact upload for every
   already-buildable Windows/macOS matrix entry. Add Android configuration
   presets only when a valid Qt Android toolchain is supplied; do not create a
   knowingly broken CI job.
5. Run the script test, configure/build the release-metadata target, focused
   C++ tests, and the default suite.

## Task 4: Evidence and integration

**Files**

- Create: `docs/superpowers/reports/2026-07-19-r4-01-verification.md`
- Modify: `README.md`

1. Record exact commands and outputs, separating local simulated release
   evidence from absent macOS/Android physical or signing gates.
2. Review the diff for credentials, platform-header leakage into the shared
   layer, and accidental changes to user-owned files.
3. Commit the R4-01 branch only after the recorded checks pass.

