# Full Motion Tracking and Retargeting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive human and animal avatars from real webcam face, gaze, mouth, head, upper-body, and two-hand motion, with calibration, confidence-aware smoothing, local audio lip-sync fallback, deterministic replay, and family-specific retargeting.

**Architecture:** New provider-neutral `AvatarMotionFrame` value types carry typed face, pose, and hand data on the project timebase. A pinned MediaPipe Tasks bridge supplies full video tracking while the existing OpenSeeFace path adapts into the same face subset. Calibration and dropout policy run above providers, audio analysis supplies visemes when face tracking is unavailable, and `AvatarRigRetargeter` evaluates the authored family profiles from the animal-rig plan. The existing `ExpressionParameters`, `ITrackingProvider`, telemetry, and render pipeline remain supported through explicit compatibility adapters during migration.

**Tech Stack:** C++20, MediaPipe v0.10.35 (`f8ef212d5c962c0e853db7e59d217056b187084b`, source archive SHA-256 `6b0e8490ab7f0f783937d4b25486cff469431adfe05c6f0217a5e63f68532113`, Apache-2.0), MediaPipe Face/Pose/Hand Landmarker task bundles with fixed hashes, nlohmann/json 3.11.3, GoogleTest 1.15.2.

## Global Constraints

- Requires completion of `2026-07-24-animal-rig-families.md`.
- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- All inference and audio analysis run locally. No camera frame, audio sample,
  landmark, calibration, or motion telemetry leaves the device unless a later
  explicit user feature and consent contract are separately approved.
- Timestamps come from `media::VideoFrame` and `media::AudioBlock`; providers
  never substitute wall-clock time.
- Face, pose, left hand, and right hand each carry independent presence,
  confidence, and last-valid timestamps. One missing subsystem does not zero
  unrelated valid motion.
- All normalized scalars are finite and clamped to documented ranges before
  crossing the provider boundary. All rotations are normalized quaternions.
- Multi-person input selects one stable primary subject using confidence,
  screen area, and temporal identity. It never jumps to a second person from a
  single frame.
- MediaPipe live-stream backpressure may drop inference inputs, but the most
  recent frame wins and output timestamps remain monotonic.
- Existing OpenSeeFace UDP/process support remains usable for face/head tracking
  and is never described as supplying body or hand data.
- `ExpressionParameters` remains source-compatible until all existing consumers
  migrate; `LegacyExpressionAdapter` is the single loss-aware conversion point.
- Camera dropout uses an explicit state machine: short hold, blend to neutral or
  tracked remainder, then audio visemes and idle. Reacquisition blends instead
  of snapping.
- Audio fallback never invents gaze, body, or hand tracking. It supplies
  voice-energy and bounded viseme estimates only.
- Retargeting evaluates the exact versioned `RigRetargetProfile` for the
  selected rig family and reports missing bindings rather than guessing names.
- Recorded motion contains no raw camera frame or raw audio. It stores typed
  normalized channels, confidence, source, calibration/profile hashes, and
  deterministic secondary-motion seed.
- Desktop tracking sub-budget: p95 inference plus normalization at 720p is
  25 ms on RTX 3060 and 32 ms on Apple M1. Snapdragon 8 Gen 1 sub-budget is
  55 ms at the mobile tracking tier. End-to-end render budgets remain owned by
  the launch qualification plan.

---

## Pinned Model Bundles

| Task | Official URL | Bytes | SHA-256 |
|---|---|---:|---|
| Face Landmarker float16 v1 | `https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task` | 3,758,596 | `64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff` |
| Pose Landmarker Full float16 v1 | `https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/1/pose_landmarker_full.task` | 9,398,198 | `5134a3aad27a58b93da0088d431f366da362b44e3ccfbe3462b3827a839011b1` |
| Hand Landmarker float16 v1 | `https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task` | 7,819,105 | `fbc2a30080c3c557093b5ddfc334698132eb341044ccee322ccf8bcf3607cde1` |

The model files receive separate `legal/OSS_BOM.csv` entries containing their
official source URL, hash, accompanying license evidence, notice requirement,
and review date. A code license entry alone cannot approve these model files.

## File Structure

```text
scripts/
  bootstrap_mediapipe.ps1
  bootstrap_mediapipe_models.ps1
  verify_mediapipe_runtime.ps1
third_party/mediapipe_bridge/
  BUILD.bazel
  creator_mediapipe_bridge.h/.cc
src/avatar/
  AvatarMotionTypes.h
  IFullMotionTrackingProvider.h
  LegacyExpressionAdapter.h/.cpp
  LegacyTrackingAdapter.h/.cpp
  FullMotionCalibrationProfile.h/.cpp
  FullMotionCalibrationCapture.h/.cpp
  AvatarMotionFilter.h/.cpp
  AvatarTrackingStateMachine.h/.cpp
  AudioVisemeEstimator.h/.cpp
  AvatarMotionFusion.h/.cpp
  RigControlFrame.h
  AvatarRigRetargeter.h/.cpp
  AvatarMotionFrameCodec.h/.cpp
  AvatarMotionFrameNdjsonSink.h/.cpp
  AvatarMotionFramePlayback.h/.cpp
src/avatar/mediapipe/
  CMakeLists.txt
  MediaPipeRuntimeManifest.h/.cpp
  MediaPipeImageBridge.h/.cpp
  MediaPipeFullMotionProvider.h/.cpp
src/avatar/openseeface/
  OpenSeeFaceFullMotionAdapter.h/.cpp
schemas/
  avatar-motion-frame.schema.json
  full-motion-calibration.schema.json
tests/avatar/
  AvatarMotionTypesTest.cpp
  LegacyExpressionAdapterTest.cpp
  FullMotionCalibrationTest.cpp
  AvatarMotionFilterTest.cpp
  AvatarTrackingStateMachineTest.cpp
  AudioVisemeEstimatorTest.cpp
  AvatarMotionFusionTest.cpp
  AvatarRigRetargeterTest.cpp
  AvatarMotionFrameCodecTest.cpp
tests/avatar/mediapipe/
  MediaPipeRuntimeManifestTest.cpp
  MediaPipeImageBridgeTest.cpp
  MediaPipeFullMotionProviderTest.cpp
tests/avatar/openseeface/
  OpenSeeFaceFullMotionAdapterTest.cpp
tests/scripts/
  MediaPipeBootstrapPolicyTest.ps1
tests/acceptance/
  FullMotionTrackingAcceptanceTest.cpp
```

## Task 1: Introduce typed full-motion values without breaking face-only consumers

**Files:**
- Create: `src/avatar/AvatarMotionTypes.h`
- Create: `src/avatar/IFullMotionTrackingProvider.h`
- Create: `src/avatar/LegacyExpressionAdapter.h`
- Create: `src/avatar/LegacyExpressionAdapter.cpp`
- Create: `src/avatar/LegacyTrackingAdapter.h`
- Create: `src/avatar/LegacyTrackingAdapter.cpp`
- Create: `src/avatar/openseeface/OpenSeeFaceFullMotionAdapter.h`
- Create: `src/avatar/openseeface/OpenSeeFaceFullMotionAdapter.cpp`
- Create: `tests/avatar/AvatarMotionTypesTest.cpp`
- Create: `tests/avatar/LegacyExpressionAdapterTest.cpp`
- Create: `tests/avatar/openseeface/OpenSeeFaceFullMotionAdapterTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

struct MotionVec3 {
    float x;
    float y;
    float z;
    friend bool operator==(const MotionVec3&, const MotionVec3&) = default;
};

struct MotionQuaternion {
    float x;
    float y;
    float z;
    float w;
    friend bool operator==(const MotionQuaternion&,
                           const MotionQuaternion&) = default;
};

enum class FaceChannel {
    EyeBlinkLeft, EyeBlinkRight, EyeLookInLeft, EyeLookOutLeft,
    EyeLookUpLeft, EyeLookDownLeft, EyeLookInRight, EyeLookOutRight,
    EyeLookUpRight, EyeLookDownRight, BrowInnerUp, BrowOuterUpLeft,
    BrowOuterUpRight, BrowDownLeft, BrowDownRight, JawOpen, MouthClose,
    MouthFunnel, MouthPucker, MouthSmileLeft, MouthSmileRight,
    MouthFrownLeft, MouthFrownRight, MouthDimpleLeft, MouthDimpleRight,
    MouthStretchLeft, MouthStretchRight, MouthPressLeft, MouthPressRight,
    MouthUpperUpLeft, MouthUpperUpRight, MouthLowerDownLeft,
    MouthLowerDownRight, MouthShrugUpper, MouthShrugLower, CheekPuff,
    CheekSquintLeft, CheekSquintRight, NoseSneerLeft, NoseSneerRight,
    Count
};

enum class BodyJoint {
    Nose, EyeLeft, EyeRight, EarLeft, EarRight,
    ShoulderLeft, ShoulderRight, ElbowLeft, ElbowRight,
    WristLeft, WristRight, PinkyLeft, PinkyRight,
    IndexLeft, IndexRight, ThumbLeft, ThumbRight,
    HipLeft, HipRight, KneeLeft, KneeRight, AnkleLeft, AnkleRight,
    HeelLeft, HeelRight, FootIndexLeft, FootIndexRight, Count
};

enum class HandJoint {
    Wrist, ThumbCmc, ThumbMcp, ThumbIp, ThumbTip,
    IndexMcp, IndexPip, IndexDip, IndexTip,
    MiddleMcp, MiddlePip, MiddleDip, MiddleTip,
    RingMcp, RingPip, RingDip, RingTip,
    PinkyMcp, PinkyPip, PinkyDip, PinkyTip, Count
};

template <typename T>
struct MotionChannel {
    T value{};
    float confidence{0.0F};
    bool present{false};
    friend bool operator==(const MotionChannel&,
                           const MotionChannel&) = default;
};

struct FaceMotion {
    std::array<MotionChannel<float>,
               static_cast<std::size_t>(FaceChannel::Count)> channels;
    MotionChannel<MotionQuaternion> headRotation;
    MotionChannel<MotionVec3> headTranslationMeters;
    friend bool operator==(const FaceMotion&, const FaceMotion&) = default;
};

struct BodyJointMotion {
    MotionVec3 imagePosition;
    MotionVec3 worldPositionMeters;
    float visibility;
    float presence;
    friend bool operator==(const BodyJointMotion&,
                           const BodyJointMotion&) = default;
};

struct BodyPose {
    std::array<BodyJointMotion,
               static_cast<std::size_t>(BodyJoint::Count)> joints;
    float confidence;
    bool present;
    friend bool operator==(const BodyPose&, const BodyPose&) = default;
};

struct HandPose {
    std::array<BodyJointMotion,
               static_cast<std::size_t>(HandJoint::Count)> joints;
    float confidence;
    bool present;
    friend bool operator==(const HandPose&, const HandPose&) = default;
};

struct AudioMotion {
    std::array<float, 5> visemesAiuEO;
    float voiceEnergy;
    float confidence;
    bool present;
    friend bool operator==(const AudioMotion&, const AudioMotion&) = default;
};

enum class MotionSource { MediaPipe, OpenSeeFace, AudioFallback, Playback };

struct AvatarMotionFrame {
    core::TimestampNs timestamp;
    FaceMotion face;
    BodyPose body;
    HandPose leftHand;
    HandPose rightHand;
    AudioMotion audio;
    MotionSource source;
    std::uint64_t subjectId;
    std::uint64_t secondaryMotionSeed;
    friend bool operator==(const AvatarMotionFrame&,
                           const AvatarMotionFrame&) = default;
};

struct FullMotionTrackingResult {
    core::TimestampNs timestamp;
    std::vector<AvatarMotionFrame> candidates;
    AvatarProviderId provider;
};

class IFullMotionTrackingProvider {
public:
    virtual ~IFullMotionTrackingProvider() = default;
    IFullMotionTrackingProvider(const IFullMotionTrackingProvider&) = delete;
    IFullMotionTrackingProvider& operator=(const IFullMotionTrackingProvider&) = delete;
    [[nodiscard]] virtual AvatarProviderId providerId() const = 0;
    [[nodiscard]] virtual core::Result<FullMotionTrackingResult> process(
        const media::VideoFrame& frame) = 0;
protected:
    IFullMotionTrackingProvider() = default;
};

class LegacyExpressionAdapter final {
public:
    static ExpressionParameters toLegacy(const AvatarMotionFrame& frame);
    static AvatarMotionFrame fromLegacy(const TrackingResult& result,
                                        AvatarProviderId provider);
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing invariant and compatibility tests**

```cpp
TEST(AvatarMotionTypesTest, RejectsNonFiniteAndOutOfRangeProviderValues) {
    auto frame = validFrame();
    frame.face.channels[index(FaceChannel::JawOpen)].value = NAN;
    EXPECT_EQ(validateAvatarMotionFrame(frame).error().code(),
              core::ErrorCode::InvalidArgument);
    frame = validFrame();
    frame.leftHand.joints[index(HandJoint::IndexTip)].presence = 1.2F;
    EXPECT_EQ(validateAvatarMotionFrame(frame).error().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(LegacyExpressionAdapterTest, PreservesExistingFaceAndHeadMeanings) {
    const auto legacy = LegacyExpressionAdapter::toLegacy(fullFaceFixture());
    EXPECT_FLOAT_EQ(legacy.mouthOpen, 0.75F);
    EXPECT_FLOAT_EQ(legacy.eyeOpenLeft, 0.8F);
    EXPECT_FLOAT_EQ(legacy.headYaw, -0.25F);
}

TEST(OpenSeeFaceFullMotionAdapterTest, DoesNotClaimBodyOrHands) {
    const auto frame = adaptOpenSeeFace(openSeeFaceFixture());
    EXPECT_TRUE(frame.face.headRotation.present);
    EXPECT_FALSE(frame.body.present);
    EXPECT_FALSE(frame.leftHand.present);
    EXPECT_FALSE(frame.rightHand.present);
}
```

- [ ] **Step 2: Run and prove full-motion type tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarMotionTypesTest|LegacyExpressionAdapterTest|OpenSeeFaceFullMotionAdapterTest" --output-on-failure
```

Expected: the new values and adapters do not exist.

- [ ] **Step 3: Implement validated values and explicit legacy loss**

Implement finite/range validation, identity quaternion defaults, typed enum
index helpers with compile-time size checks, and a stable coordinate contract:
image `x/y` in `[0,1]`, image `z` relative to image width, world coordinates in
meters, right-handed `+x` right, `+y` up, `+z` toward the viewer. Confidence,
visibility, and presence are in `[0,1]`.

`LegacyExpressionAdapter::toLegacy` derives eye-open from one minus blink,
combines left/right mouth channels by documented averages, converts normalized
head quaternion to clamped yaw/pitch/roll, and discards body/hands explicitly.
`fromLegacy` marks only available face/head fields present. The OpenSeeFace
adapter calls `fromLegacy`, preserves UDP sample timestamps, and does not create
body, hand, gaze-depth, or audio confidence.

- [ ] **Step 4: Run new and all existing avatar-motion tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarMotionTypesTest|LegacyExpressionAdapterTest|OpenSeeFaceFullMotionAdapterTest|AvatarMotionPipelineTest|Expression.*Test" --output-on-failure
```

Expected: new invariants pass and face-only behavior remains byte-compatible at
the legacy serializer boundary.

- [ ] **Step 5: Commit the compatible full-motion contract**

```powershell
git add src/avatar/AvatarMotionTypes.h src/avatar/IFullMotionTrackingProvider.h src/avatar/LegacyExpressionAdapter.h src/avatar/LegacyExpressionAdapter.cpp src/avatar/LegacyTrackingAdapter.h src/avatar/LegacyTrackingAdapter.cpp src/avatar/openseeface/OpenSeeFaceFullMotionAdapter.h src/avatar/openseeface/OpenSeeFaceFullMotionAdapter.cpp tests/avatar/AvatarMotionTypesTest.cpp tests/avatar/LegacyExpressionAdapterTest.cpp tests/avatar/openseeface/OpenSeeFaceFullMotionAdapterTest.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): add typed full motion tracking contract"
```

## Task 2: Pin MediaPipe source, models, and platform runtime

**Files:**
- Create: `scripts/bootstrap_mediapipe.ps1`
- Create: `scripts/bootstrap_mediapipe_models.ps1`
- Create: `scripts/verify_mediapipe_runtime.ps1`
- Create: `third_party/mediapipe_bridge/BUILD.bazel`
- Create: `third_party/mediapipe_bridge/creator_mediapipe_bridge.h`
- Create: `third_party/mediapipe_bridge/creator_mediapipe_bridge.cc`
- Create: `src/avatar/mediapipe/CMakeLists.txt`
- Create: `src/avatar/mediapipe/MediaPipeRuntimeManifest.h`
- Create: `src/avatar/mediapipe/MediaPipeRuntimeManifest.cpp`
- Create: `tests/avatar/mediapipe/MediaPipeRuntimeManifestTest.cpp`
- Create: `tests/scripts/MediaPipeBootstrapPolicyTest.ps1`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `legal/OSS_BOM.csv`

**Interfaces:**
- Produces: `CS_ENABLE_MEDIAPIPE`, `CS_MEDIAPIPE_ROOT`,
  `MediaPipeRuntimeManifest::loadAndVerify(root)`, staged Face/Pose/Hand models.
- Consumed by: `MediaPipeFullMotionProvider` and release packaging.

- [ ] **Step 1: Add the failing source/model pin policy test**

```powershell
$source = Get-Content scripts/bootstrap_mediapipe.ps1 -Raw
$models = Get-Content scripts/bootstrap_mediapipe_models.ps1 -Raw
if ($source -notmatch 'v0\.10\.35' -or
    $source -notmatch 'f8ef212d5c962c0e853db7e59d217056b187084b' -or
    $source -notmatch '6b0e8490ab7f0f783937d4b25486cff469431adfe05c6f0217a5e63f68532113') {
    throw 'MediaPipe source pin is incomplete'
}
foreach ($hash in @(
    '64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff',
    '5134a3aad27a58b93da0088d431f366da362b44e3ccfbe3462b3827a839011b1',
    'fbc2a30080c3c557093b5ddfc334698132eb341044ccee322ccf8bcf3607cde1')) {
    if ($models -notmatch $hash) { throw "Missing task-model pin $hash" }
}
```

- [ ] **Step 2: Run and prove bootstrap policy fails**

Run:

```powershell
pwsh -NoProfile -File tests/scripts/MediaPipeBootstrapPolicyTest.ps1
```

Expected: the bootstrap scripts do not exist.

- [ ] **Step 3: Implement reproducible bridge and model staging**

`bootstrap_mediapipe.ps1` downloads the official v0.10.35 source archive,
verifies
`6b0e8490ab7f0f783937d4b25486cff469431adfe05c6f0217a5e63f68532113`,
verifies the extracted Git commit metadata against
`f8ef212d5c962c0e853db7e59d217056b187084b`, applies only checked-in bridge
build files, then builds release C APIs for `windows-x64`, `macos-universal`, or
`android-arm64` in hermetic Bazel output roots. It stages libraries, headers,
Apache notice, transitive notices, compiler/SDK identity, ABI exports, and
per-file hashes under `build/mediapipe/prefix/<target>`.

The checked-in bridge exposes opaque Face/Pose/Hand task handles, create,
process-live-frame, poll-result, reset, and destroy functions through a C ABI.
Every result owns fixed-count typed arrays plus explicit counts/confidences;
the caller supplies output buffers, so no C++ standard-library or protobuf type
crosses the shared-library boundary. `BUILD.bazel` pins the three MediaPipe
Tasks targets and limits visibility to this bridge.

`bootstrap_mediapipe_models.ps1` downloads the three exact URLs in the pinned
model table, verifies byte count and SHA-256 before publication, stages them
read-only, and writes their source/license evidence into
`mediapipe-model-manifest.json`. It fails if an approved model-rights entry is
missing from `legal/OSS_BOM.csv`.

`MediaPipeRuntimeManifest` verifies target, architecture, source commit,
library hashes, required C exports, model byte counts/hashes, and notice files
at startup. CMake configuration fails when runtime and target disagree.

- [ ] **Step 4: Bootstrap and verify the Windows runtime**

Run:

```powershell
pwsh -NoProfile -File scripts/bootstrap_mediapipe.ps1 -Target windows-x64
pwsh -NoProfile -File scripts/bootstrap_mediapipe_models.ps1
pwsh -NoProfile -File scripts/verify_mediapipe_runtime.ps1 -Target windows-x64
pwsh -NoProfile -File tests/scripts/MediaPipeBootstrapPolicyTest.ps1
cmake --preset windows-debug -DCS_ENABLE_MEDIAPIPE=ON -DCS_MEDIAPIPE_ROOT="$PWD/build/mediapipe/prefix/windows-x64"
cmake --build --preset windows-debug --target cs_mediapipe_runtime_tests
ctest --test-dir build/windows-debug -R MediaPipeRuntimeManifestTest --output-on-failure
```

Expected: source, models, bridge libraries, exports, target ABI, and notices all
verify; changing one model or library byte makes the manifest test fail.

- [ ] **Step 5: Commit the reproducible tracking runtime**

```powershell
git add scripts/bootstrap_mediapipe.ps1 scripts/bootstrap_mediapipe_models.ps1 scripts/verify_mediapipe_runtime.ps1 third_party/mediapipe_bridge/BUILD.bazel third_party/mediapipe_bridge/creator_mediapipe_bridge.h third_party/mediapipe_bridge/creator_mediapipe_bridge.cc src/avatar/mediapipe/CMakeLists.txt src/avatar/mediapipe/MediaPipeRuntimeManifest.h src/avatar/mediapipe/MediaPipeRuntimeManifest.cpp tests/avatar/mediapipe/MediaPipeRuntimeManifestTest.cpp tests/scripts/MediaPipeBootstrapPolicyTest.ps1 CMakeLists.txt CMakePresets.json src/avatar/CMakeLists.txt tests/CMakeLists.txt legal/OSS_BOM.csv
git commit -m "build(avatar): pin MediaPipe full motion runtime"
```

## Task 3: Implement real face, pose, and two-hand inference

**Files:**
- Create: `src/avatar/mediapipe/MediaPipeImageBridge.h`
- Create: `src/avatar/mediapipe/MediaPipeImageBridge.cpp`
- Create: `src/avatar/mediapipe/MediaPipeFullMotionProvider.h`
- Create: `src/avatar/mediapipe/MediaPipeFullMotionProvider.cpp`
- Create: `tests/avatar/mediapipe/MediaPipeImageBridgeTest.cpp`
- Create: `tests/avatar/mediapipe/MediaPipeFullMotionProviderTest.cpp`
- Create: `tests/avatar/mediapipe/MediaPipeTrackingBackpressureTest.cpp`
- Modify: `src/avatar/mediapipe/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar::mediapipe {

enum class TrackingQualityTier { DesktopHigh, DesktopBalanced, Mobile };

struct MediaPipeProviderOptions {
    TrackingQualityTier quality;
    std::uint32_t maxSubjects{2};
    float minFaceConfidence{0.55F};
    float minPoseConfidence{0.55F};
    float minHandConfidence{0.50F};
};

class MediaPipeImageBridge final {
public:
    core::Result<MediaPipeImageView> map(
        const media::VideoFrame& frame,
        std::span<std::byte> conversionScratch) const;
};

class MediaPipeFullMotionProvider final :
    public avatar::IFullMotionTrackingProvider {
public:
    static core::Result<std::unique_ptr<MediaPipeFullMotionProvider>> create(
        const std::filesystem::path& runtimeRoot,
        const MediaPipeProviderOptions& options);
    AvatarProviderId providerId() const override;
    core::Result<FullMotionTrackingResult> process(
        const media::VideoFrame& frame) override;
};

} // namespace creator::avatar::mediapipe
```

- [ ] **Step 1: Add failing pixel and inference tests**

```cpp
TEST(MediaPipeImageBridgeTest, ConvertsBgraNv12AndP010WithExactVisibleRect) {
    for (const auto fixture : {bgraFixture(), nv12Fixture(), p010Fixture()}) {
        const auto image = bridge().map(fixture.frame, scratch());
        ASSERT_TRUE(image.hasValue()) << image.error().message();
        EXPECT_EQ(image.value().width, fixture.visibleWidth);
        EXPECT_EQ(image.value().height, fixture.visibleHeight);
        EXPECT_EQ(core::sha256(image.value().rgbBytes), fixture.expectedRgbHash);
    }
}

TEST(MediaPipeFullMotionProviderTest, ReadsPixelsAndReturnsFacePoseAndTwoHands) {
    auto provider = openTestProvider();
    const auto result = provider->process(loadConsentFixture("speaker-wave.bgra"));
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_EQ(result.value().candidates.size(), 1U);
    const auto& frame = result.value().candidates.front();
    EXPECT_TRUE(frame.face.headRotation.present);
    EXPECT_TRUE(frame.body.present);
    EXPECT_TRUE(frame.leftHand.present);
    EXPECT_TRUE(frame.rightHand.present);
}

TEST(MediaPipeFullMotionProviderTest, ChangedPixelsChangeTrackingOutput) {
    auto provider = openTestProvider();
    const auto first = provider->process(
        frameAt(core::TimestampNs{core::DurationNs{0}}, neutralPixels()));
    const auto second = provider->process(
        frameAt(core::TimestampNs{core::DurationNs{33'000'000}}, wavePixels()));
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_NE(hashMotion(first.value()), hashMotion(second.value()));
}
```

- [ ] **Step 2: Run and prove real-provider tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_mediapipe_tracking_tests
ctest --test-dir build/windows-debug -R "MediaPipeImageBridgeTest|MediaPipeFullMotionProviderTest" --output-on-failure
```

Expected: bridge and provider types are absent.

- [ ] **Step 3: Implement live-stream inference and typed normalization**

The image bridge uses zero-copy only when the staged MediaPipe bridge supports
the native handle and color format; otherwise it converts the visible rectangle
to a reused RGB/RGBA buffer and reports conversion time. Rotation and mirroring
are explicit options, not hidden in the coordinate mapper.

The provider creates Face Landmarker with blendshapes and facial transformation
matrices, Pose Landmarker Full with world landmarks, and Hand Landmarker with
two hands in live-stream mode. It uses one bounded latest-frame queue per task,
converts model timestamps back to the exact project timestamp, joins task
results within 35 ms, and returns partial valid subsystems when another task
drops a frame.

It maps the 52 MediaPipe face coefficients by a compile-time audited table into
the typed canonical subset, maps pose and all 21 hand landmarks by enum,
normalizes coordinate handedness and meters, classifies handedness after mirror
correction, validates every output, and discards an invalid subsystem instead
of forwarding non-finite values. Stable subject IDs use a Hungarian match over
face/torso screen boxes and landmarks with a three-frame confirmation rule.

- [ ] **Step 4: Run provider, malformed-output, and backpressure tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_mediapipe_tracking_tests
ctest --test-dir build/windows-debug -R "MediaPipeImageBridgeTest|MediaPipeFullMotionProviderTest" --output-on-failure
ctest --test-dir build/windows-debug -R MediaPipeTrackingBackpressureTest --output-on-failure
```

Expected: real fixture pixels yield face/body/two-hand motion, altered pixels
alter results, timestamps are monotonic, partial results remain honest, and a
burst keeps bounded memory while processing the newest input.

- [ ] **Step 5: Commit real full-motion inference**

```powershell
git add src/avatar/mediapipe/MediaPipeImageBridge.h src/avatar/mediapipe/MediaPipeImageBridge.cpp src/avatar/mediapipe/MediaPipeFullMotionProvider.h src/avatar/mediapipe/MediaPipeFullMotionProvider.cpp tests/avatar/mediapipe/MediaPipeImageBridgeTest.cpp tests/avatar/mediapipe/MediaPipeFullMotionProviderTest.cpp tests/avatar/mediapipe/MediaPipeTrackingBackpressureTest.cpp src/avatar/mediapipe/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): track face body and both hands with MediaPipe"
```

## Task 4: Calibrate face, gaze, body, and hands per user and device

**Files:**
- Create: `src/avatar/FullMotionCalibrationProfile.h`
- Create: `src/avatar/FullMotionCalibrationProfile.cpp`
- Create: `src/avatar/FullMotionCalibrationCapture.h`
- Create: `src/avatar/FullMotionCalibrationCapture.cpp`
- Create: `schemas/full-motion-calibration.schema.json`
- Create: `tests/avatar/FullMotionCalibrationTest.cpp`
- Modify: `src/avatar/CalibrationProfile.h`
- Modify: `src/avatar/CalibrationProfile.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

struct ScalarCalibration {
    float neutral;
    float minimum;
    float maximum;
    bool invert;
};

struct FullMotionCalibrationProfile {
    std::uint32_t schemaVersion;
    std::string id;
    AvatarProviderId provider;
    std::string deviceStableIdHash;
    std::map<FaceChannel, ScalarCalibration> face;
    MotionQuaternion neutralHead;
    MotionVec3 neutralShoulderCenter;
    float shoulderWidthMeters;
    float armSpanMeters;
    bool cameraMirrored;
    std::string profileSha256;
};

enum class CalibrationPose {
    Neutral, Blink, LookLeft, LookRight, LookUp, LookDown,
    MouthA, MouthI, MouthU, MouthE, MouthO, Smile, Frown,
    HeadLeft, HeadRight, HeadUp, HeadDown,
    ArmsDown, TPose, HandsOpen, HandsClosed
};

class FullMotionCalibrationCapture final {
public:
    core::Result<void> begin();
    core::Result<void> add(CalibrationPose pose,
                           const AvatarMotionFrame& frame);
    core::Result<FullMotionCalibrationProfile> finish() const;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing robust-calibration tests**

```cpp
TEST(FullMotionCalibrationTest, UsesRobustSamplesAndPreservesNeutralPose) {
    auto capture = captureWithRequiredPoseSequence();
    capture.add(CalibrationPose::Neutral, extremeOutlier());
    const auto profile = capture.finish();
    ASSERT_TRUE(profile.hasValue()) << profile.error().message();
    const auto calibrated = apply(profile.value(), neutralFixture());
    EXPECT_NEAR(channel(calibrated, FaceChannel::JawOpen), 0.0F, 0.02F);
    EXPECT_NEAR(quaternionAngle(calibrated.face.headRotation.value,
                               identityQuaternion()), 0.0F, 0.02F);
}

TEST(FullMotionCalibrationTest, RejectsMissingLowConfidenceAndWrongDeviceData) {
    EXPECT_EQ(captureMissing(CalibrationPose::HandsOpen).finish().error().code(),
              core::ErrorCode::InvalidState);
    EXPECT_EQ(loadProfileForDifferentDevice().error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove full-motion calibration fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R FullMotionCalibrationTest --output-on-failure
```

Expected: the full-motion profile and capture sequence are absent.

- [ ] **Step 3: Implement guided, versioned robust calibration**

Capture at least 45 valid frames per required pose, reject subsystem confidence
below its threshold, use median and median absolute deviation to remove
outliers, and require the pose-specific signal to exceed a documented minimum
range. Neutral establishes head rotation, gaze center, face baselines, shoulder
center, scale, and mirror mode. Maximum poses establish user ranges without
amplifying noise beyond 4x.

Persist only normalized calibration values and a SHA-256 hash of the device
stable ID, not a camera name, image, landmark history, or person identity.
`full-motion-calibration.schema.json` is strict and versioned. Existing
`CalibrationProfile` data migrates into a face-only profile whose missing body
and hand status is visible; it is not silently marked fully calibrated.

- [ ] **Step 4: Run calibration, migration, and privacy tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "FullMotionCalibrationTest|CalibrationProfileTest|CalibrationCaptureTest" --output-on-failure
```

Expected: robust captures pass, incomplete or weak poses fail with the exact
pose name, legacy face settings migrate, and persisted JSON contains no raw
frames, landmarks, unhashed device ID, or audio.

- [ ] **Step 5: Commit full-motion calibration**

```powershell
git add src/avatar/FullMotionCalibrationProfile.h src/avatar/FullMotionCalibrationProfile.cpp src/avatar/FullMotionCalibrationCapture.h src/avatar/FullMotionCalibrationCapture.cpp schemas/full-motion-calibration.schema.json tests/avatar/FullMotionCalibrationTest.cpp src/avatar/CalibrationProfile.h src/avatar/CalibrationProfile.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): calibrate face body and hands"
```

## Task 5: Smooth confidence changes and provide honest audio fallback

**Files:**
- Create: `src/avatar/AvatarMotionFilter.h`
- Create: `src/avatar/AvatarMotionFilter.cpp`
- Create: `src/avatar/AvatarTrackingStateMachine.h`
- Create: `src/avatar/AvatarTrackingStateMachine.cpp`
- Create: `src/avatar/AudioVisemeEstimator.h`
- Create: `src/avatar/AudioVisemeEstimator.cpp`
- Create: `src/avatar/AvatarMotionFusion.h`
- Create: `src/avatar/AvatarMotionFusion.cpp`
- Create: `tests/avatar/AvatarMotionFilterTest.cpp`
- Create: `tests/avatar/AvatarTrackingStateMachineTest.cpp`
- Create: `tests/avatar/AudioVisemeEstimatorTest.cpp`
- Create: `tests/avatar/AvatarMotionFusionTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class TrackingSubsystem { Face, Body, LeftHand, RightHand };
enum class SubsystemState { Tracking, Holding, Fading, Lost, Reacquiring };

struct TrackingStateSnapshot {
    std::array<SubsystemState, 4> subsystemStates;
    bool audioFallbackActive;
};

class AvatarMotionFilter final {
public:
    AvatarMotionFrame filter(const AvatarMotionFrame& input);
    void reset();
};

class AvatarTrackingStateMachine final {
public:
    TrackingStateSnapshot update(const AvatarMotionFrame& input);
    AvatarMotionFrame apply(const AvatarMotionFrame& input,
                            const AvatarMotionFrame& lastValid,
                            const AudioMotion& audio,
                            float idlePhase) const;
};

class AudioVisemeEstimator final {
public:
    core::Result<AudioMotion> process(const media::AudioBlock& block);
    void reset();
};

class AvatarMotionFusion final {
public:
    core::Result<void> pushVideo(const AvatarMotionFrame& frame);
    core::Result<void> pushAudio(const media::AudioBlock& block);
    core::Result<AvatarMotionFrame> sample(core::TimestampNs timestamp);
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing jitter, dropout, and audio tests**

```cpp
TEST(AvatarMotionFilterTest, ReducesStationaryJitterWithoutExcessLag) {
    const auto filtered = runFilter(noisyStationaryThenHeadTurn());
    EXPECT_LT(stationaryStandardDeviation(filtered),
              stationaryStandardDeviation(raw()) * 0.45F);
    EXPECT_LT(timeToReachNinetyPercent(filtered),
              core::DurationNs{100'000'000});
}

TEST(AvatarTrackingStateMachineTest, HoldsFadesFallsBackAndReacquires) {
    auto machine = makeStateMachine();
    EXPECT_EQ(stepFaceLoss(machine, core::DurationNs{100'000'000}),
              SubsystemState::Holding);
    EXPECT_EQ(stepFaceLoss(machine, core::DurationNs{300'000'000}),
              SubsystemState::Fading);
    const auto lost = outputAfterLoss(
        machine, core::DurationNs{600'000'000}, voicedAudio());
    EXPECT_TRUE(lost.audio.present);
    EXPECT_TRUE(bodyRemainsTracked(lost));
    EXPECT_TRUE(reacquisitionIsBlended(
        machine, core::DurationNs{250'000'000}));
}

TEST(AudioVisemeEstimatorTest, SilenceAndVowelsProduceBoundedDifferentResults) {
    const auto silence = estimator().process(silenceBlock());
    const auto vowelA = estimator().process(vowelABlock());
    ASSERT_TRUE(silence.hasValue());
    ASSERT_TRUE(vowelA.hasValue());
    EXPECT_LT(silence.value().voiceEnergy, 0.01F);
    EXPECT_GT(vowelA.value().visemesAiuEO[0], 0.35F);
    EXPECT_TRUE(allFiniteAndNormalized(vowelA.value()));
}
```

- [ ] **Step 2: Run and prove fusion tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarMotionFilterTest|AvatarTrackingStateMachineTest|AudioVisemeEstimatorTest|AvatarMotionFusionTest" --output-on-failure
```

Expected: filter, state machine, audio estimator, and fusion are absent.

- [ ] **Step 3: Implement per-subsystem temporal behavior**

Use confidence-adaptive One Euro filters for scalar and position channels and
shortest-path quaternion filtering for rotations. Reset a channel after
non-monotonic time or a gap above 500 ms. Apply these transitions:

| Subsystem | Hold | Fade to neutral/idle | Reacquire blend |
|---|---:|---:|---:|
| Face | 200 ms | 300 ms | 250 ms |
| Body | 300 ms | 400 ms | 300 ms |
| Each hand | 180 ms | 250 ms | 180 ms |

Confidence uses enter/exit hysteresis of 0.60/0.45 for face/body and 0.55/0.40
for hands. Each subsystem transitions independently. When face reaches `Lost`,
audio visemes and voice energy activate; gaze/head settle to neutral, body and
hands keep their own valid state, and a seeded idle phase supplies only authored
idle profile inputs.

`AudioVisemeEstimator` validates interleaved float PCM, downmixes to mono,
resamples to 16 kHz with persistent phase, applies DC removal and pre-emphasis,
computes 20 ms Hann-window FFT features with 10 ms hop, noise-gated energy,
spectral centroid, rolloff, and formant-band ratios, then estimates bounded
`A/I/U/E/O` weights. It adds no dependency on a network or speech transcript.

`AvatarMotionFusion` aligns video and audio to audio-master project time,
interpolates only between adjacent valid motion frames up to 50 ms, never
extrapolates tracked body/hand positions beyond the state-machine policy, and
returns the current state snapshot for UI diagnostics.

- [ ] **Step 4: Run deterministic signal and boundary tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarMotionFilterTest|AvatarTrackingStateMachineTest|AudioVisemeEstimatorTest|AvatarMotionFusionTest" --output-on-failure
```

Expected: jitter falls within target, step lag stays bounded, subsystem losses
remain independent, silence stays neutral, voiced fixtures produce different
visemes, and repeated runs are byte-identical.

- [ ] **Step 5: Commit smoothing and audio fallback**

```powershell
git add src/avatar/AvatarMotionFilter.h src/avatar/AvatarMotionFilter.cpp src/avatar/AvatarTrackingStateMachine.h src/avatar/AvatarTrackingStateMachine.cpp src/avatar/AudioVisemeEstimator.h src/avatar/AudioVisemeEstimator.cpp src/avatar/AvatarMotionFusion.h src/avatar/AvatarMotionFusion.cpp tests/avatar/AvatarMotionFilterTest.cpp tests/avatar/AvatarTrackingStateMachineTest.cpp tests/avatar/AudioVisemeEstimatorTest.cpp tests/avatar/AvatarMotionFusionTest.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): fuse confidence aware motion and audio visemes"
```

## Task 6: Retarget canonical motion to every rig family

**Files:**
- Create: `src/avatar/RigControlFrame.h`
- Create: `src/avatar/AvatarRigRetargeter.h`
- Create: `src/avatar/AvatarRigRetargeter.cpp`
- Create: `tests/avatar/AvatarRigRetargeterTest.cpp`
- Modify: `src/avatar/AvatarParameterMapper.h`
- Modify: `src/avatar/AvatarParameterMapper.cpp`
- Modify: `src/avatar/AvatarRenderPipeline.h`
- Modify: `src/avatar/AvatarRenderPipeline.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

struct RigElementControl {
    RigSemantic semantic;
    MotionVec3 translationMeters;
    MotionQuaternion rotation;
    float weight;
};

struct RigExpressionControl {
    RigExpressionSemantic semantic;
    float value;
};

struct RigTargetControl {
    RigTargetSemantic semantic;
    MotionVec3 positionMeters;
    MotionQuaternion rotation;
    float weight;
};

struct RigControlFrame {
    core::TimestampNs timestamp;
    std::vector<RigElementControl> elements;
    std::vector<RigExpressionControl> expressions;
    std::vector<RigTargetControl> targets;
    avatar_motion::SecondaryMotionInput secondaryMotion;
};

class AvatarRigRetargeter final {
public:
    core::Result<RigControlFrame> retarget(
        const AvatarMotionFrame& motion,
        const RigDefinition& rig,
        const RigRetargetProfile& profile) const;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing human/animal retarget tests**

```cpp
TEST(AvatarRigRetargeterTest, DrivesFaceHeadTorsoAndBothHandsForHumanoid) {
    const auto controls = retarget(humanoidMotion(), humanoidRig(), humanoidProfile());
    ASSERT_TRUE(controls.hasValue()) << controls.error().message();
    EXPECT_TRUE(hasExpression(controls.value(), RigExpressionSemantic::MouthA));
    EXPECT_TRUE(hasElement(controls.value(), RigSemantic::Chest));
    EXPECT_TRUE(hasTarget(controls.value(), RigTargetSemantic::HandLeft));
    EXPECT_TRUE(hasTarget(controls.value(), RigTargetSemantic::HandRight));
}

TEST(AvatarRigRetargeterTest, ConvertsHandsToPawsAndWingsByAuthoredProfile) {
    const auto quad = retarget(wavingMotion(), quadrupedRig(), quadrupedProfile());
    const auto bird = retarget(wavingMotion(), avianRig(), avianProfile());
    ASSERT_TRUE(quad.hasValue());
    ASSERT_TRUE(bird.hasValue());
    EXPECT_TRUE(hasTarget(quad.value(), RigTargetSemantic::FrontPawLeft));
    EXPECT_TRUE(hasTarget(bird.value(), RigTargetSemantic::WingLeft));
    EXPECT_FALSE(hasTarget(bird.value(), RigTargetSemantic::HandLeft));
}
```

- [ ] **Step 2: Run and prove retargeting tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R AvatarRigRetargeterTest --output-on-failure
```

Expected: control-frame and retargeter types are absent.

- [ ] **Step 3: Implement profile execution and runtime mapping**

Evaluate `RigRetargetProfile` rules in stable output-semantic order. Resolve
typed `MotionInputSemantic` values from calibrated motion, run each declared
operator with finite arithmetic, blend only where explicitly authored, clamp to
profile ranges, and verify every output exists in the exact `RigDefinition`.
Missing optional inputs use the rule's neutral fallback; missing required
outputs return `ErrorCode::InvalidArgument` with the stable avatar issue code
`avatar.motion.missing_required_output`.

Compute torso and hand targets relative to calibrated shoulder center and scale.
Use analytical two-bone IK targets only at the renderer adapter boundary;
`RigControlFrame` remains engine-neutral. Derive acceleration and angular
velocity for `SecondaryMotionInput` from filtered samples with bounded
derivatives.

Extend `AvatarParameterMapper` to map semantic controls to compiled Inochi2D
parameters or 3D binding IDs. Keep the legacy `map(ExpressionParameters)`
overload by converting through `LegacyExpressionAdapter`; do not maintain a
second set of expression formulas.

- [ ] **Step 4: Run family matrix and renderer integration tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarRigRetargeterTest|AvatarParameterMapperTest|AvatarRenderPipelineTest|AnimalRigFamiliesAcceptanceTest" --output-on-failure
```

Expected: all family outputs are present, bounded, deterministic, and connected
to real renderer controls; missing mappings fail before rendering.

- [ ] **Step 5: Commit full-motion retargeting**

```powershell
git add src/avatar/RigControlFrame.h src/avatar/AvatarRigRetargeter.h src/avatar/AvatarRigRetargeter.cpp tests/avatar/AvatarRigRetargeterTest.cpp src/avatar/AvatarParameterMapper.h src/avatar/AvatarParameterMapper.cpp src/avatar/AvatarRenderPipeline.h src/avatar/AvatarRenderPipeline.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): retarget full motion across rig families"
```

## Task 7: Record, migrate, and replay deterministic full motion

**Files:**
- Create: `src/avatar/AvatarMotionFrameCodec.h`
- Create: `src/avatar/AvatarMotionFrameCodec.cpp`
- Create: `src/avatar/AvatarMotionFrameNdjsonSink.h`
- Create: `src/avatar/AvatarMotionFrameNdjsonSink.cpp`
- Create: `src/avatar/AvatarMotionFramePlayback.h`
- Create: `src/avatar/AvatarMotionFramePlayback.cpp`
- Create: `schemas/avatar-motion-frame.schema.json`
- Create: `tests/avatar/AvatarMotionFrameCodecTest.cpp`
- Create: `tests/avatar/AvatarMotionFramePlaybackTest.cpp`
- Modify: `src/avatar/AvatarMotionSerializer.cpp`
- Modify: `src/avatar/AvatarMotionPlayback.cpp`
- Modify: `schemas/event.schema.json`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

struct AvatarMotionRecordingHeader {
    std::uint32_t schemaVersion;
    AvatarProviderId provider;
    std::string providerVersion;
    std::string calibrationSha256;
    std::string rigProfileSha256;
    std::uint64_t secondaryMotionSeed;
};

class AvatarMotionFrameCodec final {
public:
    static core::Result<nlohmann::json> encode(
        const AvatarMotionFrame& frame);
    static core::Result<AvatarMotionFrame> decode(
        const nlohmann::json& json);
    static core::Result<AvatarMotionFrame> migrateV1(
        const AvatarMotionSample& legacy);
};

class AvatarMotionFramePlayback final {
public:
    core::Result<AvatarMotionFrame> sample(core::TimestampNs timestamp) const;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing round-trip and replay tests**

```cpp
TEST(AvatarMotionFrameCodecTest, RoundTripsAllTypedChannelsCanonically) {
    const auto encoded = AvatarMotionFrameCodec::encode(fullMotionFixture());
    ASSERT_TRUE(encoded.hasValue()) << encoded.error().message();
    const auto decoded = AvatarMotionFrameCodec::decode(encoded.value());
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(decoded.value(), fullMotionFixture());
    EXPECT_EQ(canonicalDump(encoded.value()), expectedFixtureJson());
}

TEST(AvatarMotionFrameCodecTest, MigratesLegacyAsHonestFaceOnlyMotion) {
    const auto migrated = AvatarMotionFrameCodec::migrateV1(legacySample());
    ASSERT_TRUE(migrated.hasValue());
    EXPECT_TRUE(migrated.value().face.headRotation.present);
    EXPECT_FALSE(migrated.value().body.present);
    EXPECT_FALSE(migrated.value().leftHand.present);
}

TEST(AvatarMotionFramePlaybackTest, ReproducesRendererHashes) {
    const auto first = renderReplay(recordingFixture());
    const auto second = renderReplay(recordingFixture());
    EXPECT_EQ(first.frameHashes, second.frameHashes);
}
```

- [ ] **Step 2: Run and prove recording tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarMotionFrameCodecTest|AvatarMotionFramePlaybackTest" --output-on-failure
```

Expected: v2 codec and playback are absent.

- [ ] **Step 3: Implement strict v2 motion telemetry**

The schema records fixed enum spellings, finite normalized values, independent
presence/confidence, project timestamp nanoseconds, provider/version, subject
ID, calibration/profile hashes, and secondary-motion seed. It caps one event at
128 KiB, sets `additionalProperties: false`, and includes no pixel/audio data.
Serialization emits stable enum and array order with `std::to_chars`.

The sink uses the existing append-only telemetry path and fsync policy. Playback
requires monotonic timestamps, interpolates scalars/positions linearly and
rotations by shortest-path slerp only across present compatible channels, and
applies the same dropout state machine. V1 migration creates face-only values
through `LegacyExpressionAdapter` and retains the original provider/timestamp.

- [ ] **Step 4: Run codec, corruption, migration, and replay tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarMotionFrame.*Test|AvatarMotionSerializerTest|AvatarMotionPlaybackTest" --output-on-failure
```

Expected: canonical v2 round-trips, malformed/non-monotonic data fails, v1
fixtures retain old face output, and two replays produce identical controls and
renderer hashes.

- [ ] **Step 5: Commit deterministic full-motion recording**

```powershell
git add src/avatar/AvatarMotionFrameCodec.h src/avatar/AvatarMotionFrameCodec.cpp src/avatar/AvatarMotionFrameNdjsonSink.h src/avatar/AvatarMotionFrameNdjsonSink.cpp src/avatar/AvatarMotionFramePlayback.h src/avatar/AvatarMotionFramePlayback.cpp schemas/avatar-motion-frame.schema.json tests/avatar/AvatarMotionFrameCodecTest.cpp tests/avatar/AvatarMotionFramePlaybackTest.cpp src/avatar/AvatarMotionSerializer.cpp src/avatar/AvatarMotionPlayback.cpp schemas/event.schema.json src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): record and replay deterministic full motion"
```

## Task 8: Qualify real motion, fallback, privacy, and latency

**Files:**
- Create: `tests/fixtures/avatar-motion/README.md`
- Create: `tests/acceptance/FullMotionTrackingAcceptanceTest.cpp`
- Create: `scripts/measure_avatar_tracking.ps1`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`
- Modify: `legal/ASSET_BOM.csv`

**Interfaces:**
- Consumes: MediaPipe provider, calibration, fusion, profiles, both renderers,
  recording/playback.
- Produces: full-motion tracking release gate and measurement artifacts.

- [ ] **Step 1: Add the failing end-to-end motion matrix**

```cpp
TEST(FullMotionTrackingAcceptanceTest, RealFramesDriveAllPromisedSubsystems) {
    const auto replay = trackConsentSequence("face-body-two-hands");
    ASSERT_TRUE(replay.hasValue()) << replay.error().message();
    EXPECT_TRUE(sequenceHasFaceGazeMouthHead(replay.value()));
    EXPECT_TRUE(sequenceHasUpperBody(replay.value()));
    EXPECT_TRUE(sequenceHasIndependentLeftAndRightHands(replay.value()));
    EXPECT_TRUE(eachSubsystemChangesRenderedPixels(replay.value(),
                                                   humanoid3dFixture()));
}

TEST(FullMotionTrackingAcceptanceTest, AnimalRetargetAndFallbackAreVisible) {
    const auto sequence = trackThenOccludeCameraWhileSpeaking();
    EXPECT_TRUE(quadrupedPawsMuzzleEarsTailChange(sequence));
    EXPECT_TRUE(avianWingsBeakAndTailFanChange(sequence));
    EXPECT_TRUE(audioVisemesContinueAfterFaceLoss(sequence));
    EXPECT_TRUE(reacquisitionHasNoPixelDiscontinuityAbove(acceptedThreshold()));
}

TEST(FullMotionTrackingAcceptanceTest, RecordedArtifactsContainNoRawMedia) {
    const auto recording = recordTrackedSequence();
    EXPECT_FALSE(containsImageSignature(recording));
    EXPECT_FALSE(containsPcmWindow(recording, fixtureAudio()));
    EXPECT_LT(recording.maximumEventBytes(), 128U * 1024U);
}
```

- [ ] **Step 2: Run and prove the production tracking gate fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_full_motion_tracking_acceptance_tests
ctest --test-dir build/windows-debug -R FullMotionTrackingAcceptanceTest --output-on-failure
```

Expected: consented full-motion sequences, latency harness, and production gate
are absent.

- [ ] **Step 3: Add consented representative sequences and a real benchmark**

Add documented, redistributable test sequences covering:

- neutral and maximum calibration poses;
- blink, gaze directions, five vowels, smile/frown;
- head yaw/pitch/roll and upper-body lean/turn;
- independent left/right hand waves, open/close, overlap, and temporary loss;
- light/dark backgrounds, glasses, partial occlusion, and camera loss;
- spoken audio during face loss and silence;
- one and two people entering/exiting to verify subject identity stability.

Each sequence records capture resolution, frame rate, pixel format, exact frame
hashes, consent/license evidence, demographic coverage metadata without person
identity, expected subsystem intervals, and reference motion tolerances.

`measure_avatar_tracking.ps1` runs 2,000 frames after 200 warm-up frames, records
image conversion, face, pose, hands, join/filter, total tracking time, dropped
inputs, peak resident memory, and device/driver/build identity. It emits JSON
and fails the requested tier when p95 exceeds 25 ms on RTX 3060, 32 ms on Apple
M1, or 55 ms on Snapdragon 8 Gen 1. The script reports `not-qualified` when
hardware identity does not match a named tier instead of claiming success.

- [ ] **Step 4: Run correctness, privacy, replay, and available-device gates**

Run:

```powershell
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug -R "FullMotionTrackingAcceptanceTest|MediaPipeFullMotionProviderTest|AvatarRigRetargeterTest|AvatarMotionFrame.*Test" --output-on-failure
pwsh -NoProfile -File scripts/measure_avatar_tracking.ps1 -Preset windows-debug -Tier auto -Output build/qualification/avatar-tracking.json
ctest --test-dir build/windows-debug --output-on-failure
git diff --check
```

Expected: all promised motion changes real rendered pixels, fallback and
reacquisition follow the state machine, replay hashes match, telemetry contains
no raw media, matching hardware passes its sub-budget, and unmatched hardware
is explicitly unqualified.

- [ ] **Step 5: Commit full-motion qualification**

```powershell
git add tests/fixtures/avatar-motion/README.md tests/acceptance/FullMotionTrackingAcceptanceTest.cpp scripts/measure_avatar_tracking.ps1 tests/CMakeLists.txt README.md legal/ASSET_BOM.csv
git commit -m "test(avatar): qualify real full motion tracking"
```

## Plan Completion Gate

- Real camera pixels feed pinned Face, Pose, and Hand Landmarker models; changed
  pixels change typed tracking output.
- Face, gaze, mouth, head, upper body, and both hands have independent
  presence/confidence, calibration, smoothing, dropout, and reacquisition.
- OpenSeeFace remains an honest face-only source through the same canonical
  boundary.
- Local audio analysis continues bounded visemes during face loss without
  claiming missing body, hand, or gaze tracking.
- Versioned profiles retarget the same canonical motion to human, kemonomimi,
  anthro, mascot, quadruped, and avian controls.
- V2 motion recording contains normalized controls rather than raw media and
  replays deterministically.
- Consented real sequences and matching-hardware measurements gate correctness,
  privacy, stability, and the tracking latency sub-budgets.
