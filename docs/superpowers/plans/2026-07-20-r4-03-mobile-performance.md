# R4-03 Adaptive Mobile Editing and Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Studio, Editor, export, and model storage predictable on touch devices without changing project or timeline semantics.

**Architecture:** Qt Quick owns responsive presentation. A Qt-free `MobilePerformancePolicy` converts device signals into immutable budgets, while application controllers enforce those budgets before allocating storage or starting export. Project interchange remains the existing package/schema contract and is verified by round trips, not by platform-specific forks.

**Tech Stack:** C++20, Qt 6.8/QML, Android ActivityManager/PowerManager JNI, GoogleTest, CTest, adb.

## Global Constraints

- Shared policy code remains Qt-free; Android APIs stay under `src/app/android`.
- Low-resource behavior is deterministic and visible; it never silently changes output format.
- Export remains foreground-only on Android until a durable foreground-service owner exists.
- Model installation is hash-verified, size-bounded, and atomically published.
- Both Android ABIs must package; emulator evidence does not replace the physical arm64 gate.

---

### Task 1: Adaptive touch workspace and runtime budgets

**Files:**
- Modify: `qml/Main.qml`, `qml/HomePage.qml`, `qml/StudioPage.qml`, `qml/EditorPage.qml`, `qml/ExportPage.qml`
- Create: `src/platform_release/MobilePerformancePolicy.h`, `src/platform_release/MobilePerformancePolicy.cpp`
- Create: `src/app/android/AndroidDeviceProfile.h`, `src/app/android/AndroidDeviceProfile.cpp`
- Test: `tests/platform_release/MobilePerformancePolicyTest.cpp`, `tests/app/QmlSmokeTest.cpp`, `tests/app/ExportControllerTest.cpp`

**Interfaces:**
- Produces: `MobilePerformancePolicy::create(MobileDeviceProfile)` and `ExportController::setResourceConstraints(uint32_t, bool, bool)`.

- [x] Write failing 360x640 QML, policy, foreground-cancellation, and thermal-limit tests.
- [x] Implement compact panes, device-class budgets, JNI probes, and export constraints.
- [x] Verify targeted CTest, dual-ABI APK builds, install, cold start, and non-zero Android accessibility bounds.
- [x] Commit as `afd993f` and `033b679`.

### Task 2: Model storage admission and atomic publication

**Files:**
- Create: `src/platform_release/ModelStoragePolicy.h`, `src/platform_release/ModelStoragePolicy.cpp`
- Create: `tests/platform_release/ModelStoragePolicyTest.cpp`
- Modify: `src/platform_release/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MobilePerformanceBudget::maximumModelBytes`.
- Produces: `ModelStoragePolicy::admit(ModelInstallRequest, ModelStorageSnapshot)` returning an exact staging/final-path plan.

- [x] Write tests that reject a zero-byte model, a model above the device budget, insufficient free space, a path outside the configured model root, and a hash other than 64 lowercase hexadecimal characters.
- [x] Run `scripts/studio-build-verify.ps1 -SkipTests`; expect failure because `ModelStoragePolicy.h` is absent.
- [x] Implement validation with `std::filesystem::weakly_canonical`, overflow-safe size checks, and a same-root `.part` staging path:

```cpp
struct ModelInstallPlan final {
    std::filesystem::path stagingPath;
    std::filesystem::path finalPath;
    std::string expectedSha256;
};
[[nodiscard]] core::Result<ModelInstallPlan> admit(
    const ModelInstallRequest&, const ModelStorageSnapshot&) const;
```

- [x] Run `ctest --test-dir build/windows-debug -R ModelStoragePolicyTest --output-on-failure`; all seven cases pass.
- [x] Commit with `git commit -m "feat(r4): bound mobile model storage"`.

### Task 3: Cross-platform project interchange gate

**Files:**
- Create: `tests/acceptance/R4ProjectInterchangeAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ProjectPackageStore`, `SqliteStudioStore`, and the existing timeline schema.
- Produces: one platform-neutral acceptance executable that writes, reopens, edits, and reopens the same package without platform metadata.

- [ ] Write a failing round-trip test that creates a project in an Android-labelled fixture root, adds Unicode media/title/caption state, closes it, reopens it in a desktop-labelled store, edits it, and verifies semantic equality after a second reopen.
- [ ] Register the test as `R4ProjectInterchangeAcceptance` and run it RED before fixture helpers exist.
- [ ] Implement only test fixture helpers; do not add a second package format or platform field.
- [ ] Run the acceptance test and the complete `ProjectPackageStoreTest|EditorSessionWorkerTest` set.
- [ ] Commit with `git commit -m "test(r4): prove mobile desktop project interchange"`.

## Self-review

- Touch layout, deterministic preview/export budgets, foreground rules, model storage limits, and package interchange each have a direct test gate.
- Physical thermal throttling and arm64 interaction remain explicitly recorded hardware evidence, not simulated success.
- No pricing, cloud model registry, or background service is introduced by this package.
