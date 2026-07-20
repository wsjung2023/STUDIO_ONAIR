# R4-01 Cross-Platform Release Foundation Verification

Date: 2026-07-19

Scope: R4-01 release contracts and unsigned artifact evidence only. This report
does not claim signing, notarization, store publication, or physical-device
acceptance.

## Windows build and tests

Command:

```powershell
scripts/studio-build-verify.ps1 -Preset windows-debug
```

Result:

- configure succeeded with warnings-as-errors enabled;
- the complete Windows Debug build succeeded;
- CTest passed 1,100 of 1,100 tests in 128.37 seconds;
- the build-time Qt-free boundary check passed.

## Release manifest

Commands:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tests/scripts/ReleaseManifestScriptTest.ps1 -RepositoryRoot .
cmake --build --preset windows-debug --target release-metadata
```

Result:

- initial write, atomic replacement, artifact hash validation, and tamper
  detection passed;
- `build/windows-debug/release-manifest.json` contains the product version,
  exact Git revision, target, executable path, and SHA-256;
- a tracked working-tree modification appends `-dirty` to the revision, so
  local evidence cannot masquerade as a clean commit;
- no temporary or backup manifest remained after replacement.

## Android build evidence

With Android Studio's JBR supplied as `JAVA_HOME`, both configured Qt 6.8.3
Android build trees produced Debug APKs:

- arm64-v8a: `build/android-arm64-debug/android-build/creator_studio.apk`
  (30,813,944 bytes);
- x86_64: `build/android-x86_64-debug/android-build/creator_studio.apk`
  (31,482,132 bytes).

This proves packaging for both configured ABIs. It is not installation,
permission, capture, lifecycle, thermal, or protected-surface evidence.

## Remaining external gates

- macOS Debug/Release must build and test on an Apple runner;
- Android MediaProjection and device capture require a connected emulator and
  physical arm64 device;
- signing, notarization, Play upload, and release credentials remain R4-04
  authority-dependent gates.
