# R4-02 Android Capture Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Android에서 사용자가 승인한 MediaProjection 세션을 Creator Studio의 기존 화면 캡처 포트로 안전하게 연결하고, 거부·취소·회수 시 녹화 상태가 복구 가능하게 종료되도록 한다.

**Architecture:** 공유 `cs_capture`는 Android API를 모르고 `AndroidProjectionSession`으로 세대별 상태 전이만 소유한다. Android 전용 Qt 어댑터가 Java `QtActivity` 서브클래스와 JNI를 통해 MediaProjection 승인 결과·프레임·회수 콜백을 받아 기존 `IScreenCapturePermission`/`IScreenCaptureSourceFactory`로 변환한다. Java 객체와 `VirtualDisplay`는 Android 쪽에서만 보유하고, 콜백은 세대가 일치할 때만 Qt 앱 스레드로 전달한다.

**Tech Stack:** C++20, Qt 6.8 QJniObject, Android API 35 MediaProjection/ImageReader/VirtualDisplay, Qt Quick, GoogleTest, adb emulator and physical device.

## Global Constraints

- 원본 미디어와 세그먼트 파일은 수정·삭제하지 않으며, 캡처 실패는 명시적인 오류로 전달한다.
- Android·Qt·JNI 헤더는 `src/app/android`에만 두고 `cs_capture`, `domain`, `core`에는 넣지 않는다.
- 화면 캡처의 모든 Java 콜백은 `AndroidProjectionSession` generation과 일치해야 하며, 한 generation의 회수는 한 번만 녹화 종료로 전파한다.
- 화면 프레임은 bounded latest-frame 경로(`LatestVideoFrameMailbox`)로만 프리뷰에 전달하며 UI 스레드에서 인코드하거나 파일 I/O를 하지 않는다.
- Android 35, Qt 6.8, x86_64 emulator 및 arm64-v8a physical device APK를 모두 빌드한다.
- 보호된 콘텐츠·권한 거부·OS 회수는 우회하지 않고 사용자가 이해할 수 있는 상태 메시지로 표시한다.

---

### Task 1: 승인 결과를 세대 안전하게 전달하는 Android 권한 어댑터

**Files:**
- Create: `src/app/android/AndroidScreenCaptureBackend.h`
- Create: `src/app/android/AndroidScreenCaptureBackend.cpp`
- Create: `android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java`
- Create: `tests/scripts/AndroidProjectionBridgePolicyTest.ps1`
- Modify: `android/AndroidManifest.xml`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `capture::AndroidProjectionSession::beginApprovedProjection()`, `markStreaming(generation)`, `onProjectionRevoked(generation)`.
- Produces: `creator::app::android::makeAndroidScreenCaptureBackend()` returning permission, discovery, and source-factory implementations for `ScreenCaptureController`.

- [ ] **Step 1: Write the failing bridge policy test**

```powershell
$required = @(
    'CreatorStudioActivity extends QtActivity',
    'nativeProjectionResult',
    'requestProjection',
    'makeAndroidScreenCaptureBackend',
    'defined\(ANDROID\)'
)
foreach ($text in $required) {
    if (-not (Select-String -LiteralPath $sourcePaths -Pattern $text -Quiet)) {
        throw "missing Android projection bridge boundary: $text"
    }
}
```

- [ ] **Step 2: Run the test and verify RED**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/AndroidProjectionBridgePolicyTest.ps1 -RepositoryRoot .`

Expected: FAIL because the Java activity and Android C++ adapter do not yet exist.

- [ ] **Step 3: Add the Java activity boundary**

```java
public final class CreatorStudioActivity extends QtActivity {
    public static native void nativeProjectionResult(long generation, boolean granted);
    public static native void nativeProjectionRevoked(long generation);

    public void requestProjection(long generation) {
        MediaProjectionManager manager =
            (MediaProjectionManager) getSystemService(MEDIA_PROJECTION_SERVICE);
        startActivityForResult(manager.createScreenCaptureIntent(), REQUEST_PROJECTION);
    }
}
```

The implementation stores only one pending generation, rejects a second prompt while one is outstanding, and calls `nativeProjectionResult` exactly once from `onActivityResult`.

- [ ] **Step 4: Add the Qt adapter and route the callback**

```cpp
struct AndroidScreenCaptureBackend final {
    std::unique_ptr<capture::IScreenCapturePermission> permission;
    std::unique_ptr<capture::IScreenCaptureDiscovery> discovery;
    std::unique_ptr<capture::IScreenCaptureSourceFactory> sourceFactory;
};

[[nodiscard]] AndroidScreenCaptureBackend makeAndroidScreenCaptureBackend();
```

`request()` starts the Android activity only on the Qt Android main thread. The native JNI callback queues its completion onto the `ScreenCaptureController` thread and ignores a generation that is no longer pending. The Android discovery exposes one full-display target because MediaProjection captures the user-selected display surface, not arbitrary windows.

- [ ] **Step 5: Make the manifest use the custom activity**

```xml
<activity android:name="com.studioonair.creatorstudio.CreatorStudioActivity" ...>
```

Keep the Qt application class and all current permissions unchanged. In `main.cpp`, select this backend only under `ANDROID`; desktop branches remain byte-for-byte behaviorally equivalent.

- [ ] **Step 6: Verify GREEN and build both Android ABIs**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
build/windows-debug/tests/cs_tests --gtest_filter=AndroidProjectionSessionTest.*
ctest --test-dir build/windows-debug -R AndroidProjectionBridgePolicy --output-on-failure
cmake --build --preset android-x86_64-debug --target apk
cmake --build --preset android-arm64-debug --target apk
```

Expected: every AndroidProjectionSession test passes and both APKs are generated.

- [ ] **Step 7: Commit**

```powershell
git add src/app/android android/src android/AndroidManifest.xml src/main.cpp CMakeLists.txt tests/CMakeLists.txt tests/scripts/AndroidProjectionBridgePolicyTest.ps1
git commit -m "feat(android): request MediaProjection consent safely"
```

### Task 2: 승인된 MediaProjection을 프레임 소스로 연결

**Files:**
- Modify: `src/app/android/AndroidScreenCaptureBackend.h`
- Modify: `src/app/android/AndroidScreenCaptureBackend.cpp`
- Modify: `android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java`
- Test: `tests/capture/AndroidProjectionSessionTest.cpp`

**Interfaces:**
- Consumes: Task 1 `makeAndroidScreenCaptureBackend()` and current approved generation.
- Produces: `IScreenCaptureSource` whose `start()` marks the matching projection streaming after the first valid frame and whose `stopAsync()` releases Android capture resources exactly once.

- [ ] **Step 1: Write a failing revoke/stop state test**

```cpp
TEST(AndroidProjectionSessionTest, RevokedProjectionCannotBecomeStreamingAgain) {
    AndroidProjectionSession session;
    const auto generation = session.beginApprovedProjection();
    EXPECT_TRUE(session.markStreaming(generation));
    EXPECT_TRUE(session.onProjectionRevoked(generation));
    EXPECT_FALSE(session.markStreaming(generation));
}
```

- [ ] **Step 2: Run RED**

Run: `build/windows-debug/tests/cs_tests --gtest_filter=AndroidProjectionSessionTest.RevokedProjectionCannotBecomeStreamingAgain`

Expected: failure only until the required state transition is enforced.

- [ ] **Step 3: Add ImageReader capture in Java**

```java
ImageReader reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
virtualDisplay = projection.createVirtualDisplay("CreatorStudioCapture", width, height,
    densityDpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
    reader.getSurface(), null, handler);
reader.setOnImageAvailableListener(this::onImage, handler);
```

`onImage` closes every `Image` in `finally`, copies only the required RGBA rows into a bounded direct buffer, and drops a frame when native delivery reports it cannot accept one. `MediaProjection.Callback.onStop()` emits `nativeProjectionRevoked(generation)` exactly once.

- [ ] **Step 4: Convert frames at the adapter boundary**

```cpp
void AndroidScreenCaptureSource::onRgbaFrame(std::uint64_t generation,
                                             std::span<const std::byte> rgba,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             std::int64_t timestampNs);
```

The source verifies generation, constructs `NativeScreenFrame`, then uses `ScreenCaptureFrameAssembler` before calling `IVideoFrameSink`. Invalid stride, empty bytes, or timestamps outside the mapper contract increment invalid-frame stats and do not publish malformed media.

- [ ] **Step 5: Verify GREEN**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
build/windows-debug/tests/cs_tests --gtest_filter=AndroidProjectionSessionTest.*
cmake --build --preset android-x86_64-debug --target apk
adb -s emulator-5554 install -r build/android-x86_64-debug/android-build/build/outputs/apk/debug/android-build-debug.apk
```

Expected: unit tests pass; the emulator installs and the Android permission prompt can be opened. Actual capture approval is a physical-device gate because emulator output is not evidence for protected surfaces or device compositor timing.

- [ ] **Step 6: Commit**

```powershell
git add src/app/android android/src tests/capture/AndroidProjectionSessionTest.cpp
git commit -m "feat(android): stream approved projection frames"
```

### Task 3: Interruptions, recovery, and physical acceptance

**Files:**
- Modify: `src/app/android/AndroidScreenCaptureBackend.cpp`
- Modify: `android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java`
- Create: `tests/scripts/AndroidProjectionAcceptance.ps1`
- Modify: `docs/superpowers/specs/2026-07-19-r4-cross-platform-release-design.md`

**Interfaces:**
- Consumes: Task 2 source lifecycle and `IScreenCaptureSource::stopAsync(StopCompletion)`.
- Produces: repeatable adb acceptance evidence plus a documented physical-device matrix for grant, deny, rotation/background, revoke, and restart.

- [ ] **Step 1: Write the failing acceptance policy assertion**

```powershell
$required = @('MediaProjection.Callback', 'nativeProjectionRevoked', 'stopAsync')
foreach ($text in $required) {
    if (-not (Select-String -LiteralPath $backendPath -Pattern $text -Quiet)) {
        throw "missing Android projection lifecycle boundary: $text"
    }
}
```

- [ ] **Step 2: Run RED**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/AndroidProjectionAcceptance.ps1 -RepositoryRoot .`

Expected: it fails until both Java revocation and native asynchronous stop contracts exist.

- [ ] **Step 3: Implement interruption handling**

```cpp
void AndroidScreenCaptureSource::stopAsync(StopCompletion completion) {
    // Bar frames first, request Java release once, and complete only after the
    // matching Java release callback reaches the Qt object's thread.
}
```

On activity destruction, display rotation, or Android projection revoke, the adapter stops frame delivery, ends the recorder source, preserves the durable package, and makes a later start use a new generation. It never treats a revoked projection as an ordinary successful completion.

- [ ] **Step 4: Verify script, APK, and physical-device sequence**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/AndroidProjectionAcceptance.ps1 -RepositoryRoot .
cmake --build --preset android-arm64-debug --target apk
adb devices -l
```

On the connected phone: launch Creator Studio, approve screen recording, record 60 seconds with screen + microphone, background/foreground once, stop, reopen the project, and inspect that the segment list is intact. Repeat once after denying permission and once after revoking the Android system capture notification. Record adb logcat and export the resulting package metadata as evidence.

- [ ] **Step 5: Commit**

```powershell
git add src/app/android android/src tests/scripts/AndroidProjectionAcceptance.ps1 docs/superpowers/specs/2026-07-19-r4-cross-platform-release-design.md
git commit -m "test(android): verify projection interruption recovery"
```

## Plan self-review

- Task 1 establishes permission ownership without putting Android headers below the app boundary.
- Task 2 establishes frames and bounded delivery without inventing desktop-only target semantics.
- Task 3 covers Android lifecycle revocation and physical evidence; it does not claim simulator output as device acceptance.
- Camera, microphone, playback capture, MediaCodec export, scoped storage, and adaptive editing remain subsequent R4-02/R4-03 work packages, not removed product scope.
