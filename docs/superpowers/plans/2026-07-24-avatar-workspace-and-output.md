# Avatar Workspace and Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a complete desktop/mobile Avatar workspace in which every visible edit compiles into the real 2D or 3D model, survives save/recovery, drives tracking, and reaches Studio recording, editing playback, rights-checked model export, and verified OBS/chroma broadcast output through the same renderer.

**Architecture:** A Qt-free `AvatarEditorSession` owns the working `AvatarSpec`, reversible commands, validation, compile generations, and last-good artifact. A Qt application adapter exposes typed list models and one controller to adaptive QML; a worker compiles off the GUI thread and atomically swaps the real renderer only after it opens. One timestamped `AvatarFrameHub` fans the same renderer output to the QML preview mailbox, `AvatarRenderCaptureSource`, recording/edit playback, and broadcast sinks. A separate GPL OBS source plugin consumes a documented IPC texture protocol and is never linked into the closed core; verified alpha IPC is preferred and a chroma output window is always available.

**Tech Stack:** C++20, Qt 6.8 Quick/Quick Controls/Concurrent/Multimedia, existing project-store and capture layers, Inochi2D/Filament compilers and renderers from earlier plans, OBS Studio 32.2.0 plugin SDK only in an isolated GPL subproject, GoogleTest 1.15.2, Qt Test.

## Global Constraints

- Requires completion of all prior avatar plans through
  `2026-07-24-full-motion-tracking-and-retargeting.md`.
- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- No QML component constructs an `AvatarSpec`, reads packages, compiles models,
  calls engine APIs, or opens project files directly. QML invokes controller
  methods and consumes models/properties.
- No UI success state is emitted until the requested generation compiles, its
  artifact hash verifies, and the actual renderer opens.
- A compile failure preserves the prior working spec, compiled artifact, and
  rendered preview. The UI shows the exact failing slot/asset/rights issue.
- Preview, Studio, recording, edit playback, and broadcast share the same
  `IAvatarRenderer`, compiled artifact, motion fusion, material behavior, and
  timestamp contract. They may request different resolution/LOD only.
- The preview item displays real renderer frames. A stock image, QML shape,
  screenshot, animated gradient, rigid sample mesh, or synthetic test pattern
  cannot satisfy readiness.
- Undo/redo operates on semantic edits and restores the exact previous spec,
  selection, compatibility result, rights decision, and compiled generation.
- Autosave is debounced by 750 ms, transactional, retains the last good
  snapshot, and never marks an unverified compile as the current saved artifact.
- Missing assets are never replaced automatically. The UI lists asset ID,
  version, expected hash, affected slot, and install/remove/choose-replacement
  actions.
- Randomize changes only unlocked compatible slots/values and evaluates rights
  for the chosen use case before committing the command.
- Every interactive control has an accessible name/role, keyboard or switch
  path, visible focus, and Korean/English translation.
- Android provides the same data and operations in bottom sheets and full-screen
  pickers. It may use a lower quality tier but does not omit categories,
  calibration, rights, save, export, or output controls.
- The closed core contains no OBS header, library, copied OBS source, or GPL
  binary. OBS integration is a separately built/distributed GPL-2.0-or-later
  plugin communicating over a versioned IPC boundary.
- Alpha broadcast is advertised only after a consumer handshake and frame
  validation. Otherwise output automatically exposes a user-visible chroma
  window with collision analysis.
- Model export re-runs current rights evaluation. UI state, old receipts, and
  saved rights snapshots cannot authorize export.

---

## File Structure

```text
src/avatar/
  AvatarEditCommand.h
  AvatarEditCommands.h/.cpp
  AvatarEditHistory.h/.cpp
  AvatarEditorSession.h/.cpp
  AvatarFrameHub.h/.cpp
  AvatarBroadcastProtocol.h/.cpp
  IAvatarBroadcastSink.h
  SharedAvatarBroadcastSink.h/.cpp
  ChromaCollisionAnalyzer.h/.cpp
src/avatar_2d_adapter/
  Inochi2dModelExporter.h/.cpp
src/app/
  AvatarEditorController.h/.cpp
  AvatarEditorWorker.h/.cpp
  AvatarCatalogModel.h/.cpp
  AvatarInspectorModel.h/.cpp
  AvatarIssueModel.h/.cpp
  AvatarRightsModel.h/.cpp
  AvatarPreviewItem.h/.cpp
  AvatarPreviewProducer.h/.cpp
  AvatarTrackingController.h/.cpp
  AvatarOutputController.h/.cpp
  AvatarSourceRegistry.h/.cpp
  ChromaOutputWindow.h/.cpp
src/capture/android/
  QtAndroidCameraCaptureSource.h/.cpp
  QtAndroidAudioCaptureSource.h/.cpp
android/
  AndroidManifest.xml
  res/xml/file_paths.xml
qml/
  AvatarPage.qml
  avatar/AvatarTopBar.qml
  avatar/AvatarCatalogPanel.qml
  avatar/AvatarPreviewPanel.qml
  avatar/AvatarInspectorPanel.qml
  avatar/AvatarStatusBar.qml
  avatar/AvatarRightsDialog.qml
  avatar/AvatarMissingAssetsDialog.qml
  avatar/AvatarCalibrationDialog.qml
  avatar/AvatarExportDialog.qml
  avatar/AvatarMobileSheet.qml
translations/
  creatorstudio_ko.ts
  creatorstudio_en.ts
tools/obs-avatar-source/
  CMakeLists.txt
  LICENSE
  README.md
  src/obs-avatar-source.c
  src/avatar-ipc-reader.h/.c
  tests/avatar-ipc-reader-test.cpp
scripts/
  bootstrap_obs_sdk.ps1
  build_obs_avatar_source.ps1
  verify_avatar_output.ps1
tests/avatar/
  AvatarEditHistoryTest.cpp
  AvatarEditorSessionTest.cpp
  AvatarFrameHubTest.cpp
  AvatarBroadcastProtocolTest.cpp
  ChromaCollisionAnalyzerTest.cpp
tests/avatar_2d_adapter/
  Inochi2dModelExporterTest.cpp
tests/app/
  AvatarEditorControllerTest.cpp
  AvatarModelsTest.cpp
  AvatarPreviewItemTest.cpp
  AvatarSourceRegistryTest.cpp
  AvatarOutputControllerTest.cpp
tests/qml/
  AvatarPageTest.cpp
tests/acceptance/
  AvatarWorkspaceOutputAcceptanceTest.cpp
```

## Task 1: Build reversible editor state around `AvatarSpec`

**Files:**
- Create: `src/avatar/AvatarEditCommand.h`
- Create: `src/avatar/AvatarEditCommands.h`
- Create: `src/avatar/AvatarEditCommands.cpp`
- Create: `src/avatar/AvatarEditHistory.h`
- Create: `src/avatar/AvatarEditHistory.cpp`
- Create: `src/avatar/AvatarEditorSession.h`
- Create: `src/avatar/AvatarEditorSession.cpp`
- Create: `tests/avatar/AvatarEditHistoryTest.cpp`
- Create: `tests/avatar/AvatarEditorSessionTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class AvatarEditKind {
    SelectAsset, RemoveAsset, SetMorph, SetColor, SetMaterial,
    SetPhysics, SetTracking, SetExpressionShortcut, SetSlotLocked,
    SetRepresentation, SetRigFamily, ReplaceSpec
};

struct AvatarEditRecord {
    std::string commandId;
    AvatarEditKind kind;
    nlohmann::json forward;
    nlohmann::json inverse;
};

class IAvatarEditCommand {
public:
    virtual ~IAvatarEditCommand() = default;
    virtual core::Result<AvatarSpec> apply(const AvatarSpec& before) const = 0;
    virtual AvatarEditRecord record() const = 0;
};

class AvatarEditHistory final {
public:
    core::Result<AvatarSpec> execute(const AvatarSpec& current,
                                     std::unique_ptr<IAvatarEditCommand> command);
    core::Result<AvatarSpec> undo(const AvatarSpec& current);
    core::Result<AvatarSpec> redo(const AvatarSpec& current);
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
};

enum class AvatarSessionPhase {
    Empty, Loading, Ready, Compiling, Saving, Recovering, Error
};

struct AvatarEditorSnapshot {
    AvatarSpec spec;
    std::optional<CompiledAvatarArtifact> artifact;
    AvatarRightsMatrix rights;
    std::vector<RigCompatibilityIssue> issues;
    std::uint64_t requestedGeneration;
    std::uint64_t activeGeneration;
    AvatarSessionPhase phase;
    bool dirty;
};

class AvatarEditorSession final {
public:
    core::Result<std::uint64_t> execute(
        std::unique_ptr<IAvatarEditCommand> command);
    core::Result<std::uint64_t> undo();
    core::Result<std::uint64_t> redo();
    core::Result<std::uint64_t> randomize(const AvatarRandomizeRequest& request);
    core::Result<void> acceptCompiled(
        std::uint64_t generation,
        CompiledAvatarArtifact artifact);
    core::Result<void> rejectCompiled(
        std::uint64_t generation,
        core::AppError error);
    [[nodiscard]] AvatarEditorSnapshot snapshot() const;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing command/history/session tests**

```cpp
TEST(AvatarEditHistoryTest, UndoRedoRestoresExactSemanticState) {
    auto session = sessionWith(humanSpec());
    ASSERT_TRUE(session.execute(selectHair("core.hair.layered-bob")).hasValue());
    ASSERT_TRUE(session.execute(setColor("hair.base", "#6B4EFF")).hasValue());
    const auto edited = session.snapshot().spec;
    ASSERT_TRUE(session.undo().hasValue());
    EXPECT_EQ(session.snapshot().spec.slots().at(AvatarSlot::HairFront),
              humanSpec().slots().at(AvatarSlot::HairFront));
    ASSERT_TRUE(session.redo().hasValue());
    EXPECT_EQ(session.snapshot().spec, edited);
}

TEST(AvatarEditorSessionTest, FailedCompileKeepsLastGoodArtifactAndSpecVisible) {
    auto session = readySession();
    const auto prior = session.snapshot();
    const auto generation = session.execute(selectIncompatibleOutfit()).value();
    ASSERT_TRUE(session.rejectCompiled(generation, incompatibleRigError()).hasValue());
    EXPECT_EQ(session.snapshot().activeGeneration, prior.activeGeneration);
    ASSERT_TRUE(session.snapshot().artifact.has_value());
    ASSERT_TRUE(prior.artifact.has_value());
    EXPECT_EQ(session.snapshot().artifact->contentSha256,
              prior.artifact->contentSha256);
    EXPECT_EQ(session.snapshot().artifact->modelPath,
              prior.artifact->modelPath);
    EXPECT_TRUE(session.snapshot().dirty);
    EXPECT_TRUE(hasIssueForSlot(session.snapshot(), AvatarSlot::Outerwear));
}

TEST(AvatarEditorSessionTest, RandomizeHonorsLocksCompatibilityAndRights) {
    auto session = lockedHairSession();
    ASSERT_TRUE(session.randomize(forUseKind(UseKind::Broadcast)).hasValue());
    EXPECT_EQ(session.snapshot().spec.slots().at(AvatarSlot::HairFront),
              lockedHair());
    EXPECT_TRUE(allSelectedAssetsCompatible(session.snapshot()));
    EXPECT_TRUE(
        session.snapshot().rights.forUse(UseKind::Broadcast).allowed);
}

TEST(AvatarEditorSessionTest, RepresentationSwitchKeepsSharedSemanticsAndRequiresRealAssets) {
    auto session = readyInochi2dSession();
    const auto result = session.switchRepresentation(
        AvatarRepresentation::Vrm1, confirmed3dAssetMapping());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(session.snapshot().spec.values().palette,
              readyInochi2dSession().snapshot().spec.values().palette);
    EXPECT_EQ(session.snapshot().spec.values().trackingProfileId,
              readyInochi2dSession().snapshot().spec.values().trackingProfileId);
    EXPECT_TRUE(allSelectedAssetsHaveRepresentation(
        session.snapshot().spec, AvatarRepresentation::Vrm1));
}
```

- [ ] **Step 2: Run and prove editor-domain tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarEditHistoryTest|AvatarEditorSessionTest" --output-on-failure
```

Expected: editor commands, history, and session are absent.

- [ ] **Step 3: Implement validated immutable edits and generation control**

Each command validates typed arguments, creates a new `AvatarSpec` through
`AvatarSpec::create`, records forward/inverse canonical JSON, and never mutates
the input. History is limited to 500 semantic commands and coalesces continuous
slider changes only when kind, channel, and gesture ID match.

Before a session publishes a requested generation it resolves assets, validates
part/rig compatibility, and evaluates rights for preview, commercial broadcast,
app bundle, derivative use, and model export. Errors attach stable issue codes
to exact assets/slots. A generation older than `requestedGeneration` is ignored.
Only `acceptCompiled` for the newest generation swaps the active artifact.
`rejectCompiled` keeps the last good artifact and requested spec so the user can
fix the precise edit or undo it.

Randomization builds candidates from compatible catalog entries, excludes
locked slots and scalar groups, enforces dependencies/conflicts and requested
rights, uses a recorded seed, validates the complete proposed spec, and commits
one reversible `ReplaceSpec` command.

Representation switching is one reversible command. It preserves shared body/
face/animal morph meanings, palette channels, material meanings, expressions,
physics, locks, and tracking profile; it restores the session's last explicit
selection for the destination representation when hashes are still installed.
Otherwise it presents semantically tagged compatible candidates and requires a
confirmed mapping before the command executes. It never converts 2D art to 3D,
converts a mesh to layered art, or silently substitutes the first catalog item.
Rig-family switching follows the same confirmed-candidate rule and blocks until
all required family slots/controls are satisfied.

- [ ] **Step 4: Run history, stale-result, and property tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "AvatarEditHistoryTest|AvatarEditorSessionTest|AvatarSpecTest|RigCompatibilityValidatorTest" --output-on-failure
```

Expected: exact undo/redo, coalescing, stale compile rejection, last-good
retention, locked randomization, and rights filtering pass.

- [ ] **Step 5: Commit the real editor session**

```powershell
git add src/avatar/AvatarEditCommand.h src/avatar/AvatarEditCommands.h src/avatar/AvatarEditCommands.cpp src/avatar/AvatarEditHistory.h src/avatar/AvatarEditHistory.cpp src/avatar/AvatarEditorSession.h src/avatar/AvatarEditorSession.cpp tests/avatar/AvatarEditHistoryTest.cpp tests/avatar/AvatarEditorSessionTest.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): add reversible production editor session"
```

## Task 2: Expose the catalog, inspector, issues, rights, and compiler to Qt

**Files:**
- Create: `src/app/AvatarEditorController.h`
- Create: `src/app/AvatarEditorController.cpp`
- Create: `src/app/AvatarEditorWorker.h`
- Create: `src/app/AvatarEditorWorker.cpp`
- Create: `src/app/AvatarCatalogModel.h`
- Create: `src/app/AvatarCatalogModel.cpp`
- Create: `src/app/AvatarInspectorModel.h`
- Create: `src/app/AvatarInspectorModel.cpp`
- Create: `src/app/AvatarIssueModel.h`
- Create: `src/app/AvatarIssueModel.cpp`
- Create: `src/app/AvatarRightsModel.h`
- Create: `src/app/AvatarRightsModel.cpp`
- Create: `tests/app/AvatarEditorControllerTest.cpp`
- Create: `tests/app/AvatarModelsTest.cpp`
- Create: `tests/app/AvatarEditorWorkerTest.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::app {

class AvatarEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(QString representation READ representation NOTIFY stateChanged)
    Q_PROPERTY(QString rigFamily READ rigFamily NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(qulonglong activeGeneration READ activeGeneration NOTIFY stateChanged)
    Q_PROPERTY(AvatarCatalogModel* catalog READ catalog CONSTANT)
    Q_PROPERTY(AvatarInspectorModel* inspector READ inspector CONSTANT)
    Q_PROPERTY(AvatarIssueModel* issues READ issues CONSTANT)
    Q_PROPERTY(AvatarRightsModel* rights READ rights CONSTANT)
public:
    Q_INVOKABLE void createAvatar(QString representation, QString rigFamily);
    Q_INVOKABLE void setRepresentation(QString representation,
                                       QVariantMap confirmedAssetMapping);
    Q_INVOKABLE void setRigFamily(QString rigFamily,
                                  QVariantMap confirmedAssetMapping);
    Q_INVOKABLE void openAvatar(QUrl url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs(QUrl url);
    Q_INVOKABLE void selectAsset(QString slot, QString assetId,
                                 QString version, QString variantId);
    Q_INVOKABLE void removeAsset(QString slot);
    Q_INVOKABLE void setScalar(QString name, double value, QString gestureId);
    Q_INVOKABLE void setColor(QString channel, QColor value, QString gestureId);
    Q_INVOKABLE void setMaterial(QString channel, QVariantMap values,
                                 QString gestureId);
    Q_INVOKABLE void setPhysics(QString channel, QVariantMap values,
                                QString gestureId);
    Q_INVOKABLE void setSlotLocked(QString slot, bool locked);
    Q_INVOKABLE void randomize(QString useCase, qulonglong seed);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
};

} // namespace creator::app
```

- [ ] **Step 1: Add failing controller/model contract tests**

```cpp
TEST(AvatarEditorControllerTest, EmitsReadyOnlyAfterRealRendererOpens) {
    FakeAvatarCompiler compiler;
    FakeAvatarRendererFactory renderers;
    AvatarEditorController controller{dependencies(compiler, renderers)};
    controller.createAvatar("inochi2d", "kemonomimi");
    EXPECT_TRUE(controller.busy());
    EXPECT_FALSE(controller.ready());
    compiler.completeWith(realArtifactFixture());
    EXPECT_FALSE(controller.ready());
    renderers.completeOpen();
    EXPECT_TRUE(controller.ready());
    EXPECT_EQ(controller.activeGeneration(), 1U);
}

TEST(AvatarModelsTest, ExposesSearchFilterFavoriteRecentCompatibilityAndRights) {
    auto model = catalogModelWithMixedEntries();
    model.setSearchText("layered");
    model.setFamilyFilter("avian");
    model.setFavoritesOnly(true);
    ASSERT_EQ(model.rowCount(), 1);
    const auto row = model.index(0, 0);
    EXPECT_TRUE(model.data(row, AvatarCatalogModel::CompatibleRole).toBool());
    EXPECT_TRUE(model.data(row, AvatarCatalogModel::CommercialBroadcastRole).toBool());
}
```

- [ ] **Step 2: Run and prove Qt editor adapter tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarEditorControllerTest|AvatarModelsTest" --output-on-failure
```

Expected: controller, worker, and list models are absent.

- [ ] **Step 3: Implement one queued worker and typed model roles**

`AvatarEditorWorker` lives on one owned `QThread`, receives immutable compile
requests containing generation/spec/catalog snapshot, checks cancellation
between validation/resolve/compile/open phases, and returns a
`shared_ptr<const Result<AvatarCompileCompletion>>`. No lambda captures a
controller by raw pointer. Controller destruction stops intake, cancels work,
quits, and joins.

Catalog roles include stable asset ID/version, localized name, representation,
families, slot, theme/tags, thumbnail URL, favorite/recent, installed,
compatible, locked, broadcast/derivative/bundle/export rights, attribution,
and exact disabled reason. Search uses normalized Unicode and tags. Favorites
and recent IDs persist in settings but do not copy asset data.

Inspector rows are typed as scalar, color, material, physics, tracking,
expression shortcut, or enum. Each row declares bounds, step, units, group,
enabled state, and issue text from compiled metadata/asset manifests—not a
hardcoded QML field list. Rights rows expose each use case, allow/deny,
restricting asset, attribution, and evidence date.

- [ ] **Step 4: Run thread-lifetime, stale-generation, and model tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarEditorControllerTest|AvatarModelsTest|AvatarEditorWorkerTest" --output-on-failure
```

Expected: controller readiness matches renderer readiness, stale completions are
discarded, destruction is race-free, and model filters/roles update precisely.

- [ ] **Step 5: Commit the Qt editor boundary**

```powershell
git add src/app/AvatarEditorController.h src/app/AvatarEditorController.cpp src/app/AvatarEditorWorker.h src/app/AvatarEditorWorker.cpp src/app/AvatarCatalogModel.h src/app/AvatarCatalogModel.cpp src/app/AvatarInspectorModel.h src/app/AvatarInspectorModel.cpp src/app/AvatarIssueModel.h src/app/AvatarIssueModel.cpp src/app/AvatarRightsModel.h src/app/AvatarRightsModel.cpp tests/app/AvatarEditorControllerTest.cpp tests/app/AvatarModelsTest.cpp tests/app/AvatarEditorWorkerTest.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): expose production editor models to Qt"
```

## Task 3: Fan one real renderer into preview and every frame consumer

**Files:**
- Create: `src/avatar/AvatarFrameHub.h`
- Create: `src/avatar/AvatarFrameHub.cpp`
- Create: `src/app/AvatarPreviewProducer.h`
- Create: `src/app/AvatarPreviewProducer.cpp`
- Create: `src/app/AvatarPreviewItem.h`
- Create: `src/app/AvatarPreviewItem.cpp`
- Create: `tests/avatar/AvatarFrameHubTest.cpp`
- Create: `tests/app/AvatarPreviewItemTest.cpp`
- Modify: `src/avatar/AvatarRenderPipeline.h`
- Modify: `src/avatar/AvatarRenderPipeline.cpp`
- Modify: `src/app/VideoPreviewItem.h`
- Modify: `src/app/VideoPreviewItem.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class AvatarFrameConsumer {
    WorkspacePreview, StudioPreview, Recording, EditPlayback, Broadcast
};

struct AvatarFrameRequest {
    AvatarFrameConsumer consumer;
    core::TimestampNs timestamp;
    std::uint32_t width;
    std::uint32_t height;
    AvatarQualityTier quality;
    bool cpuReadback;
};

struct AvatarFrameReceipt {
    media::VideoFrame frame;
    std::uint64_t rendererGeneration;
    std::string artifactSha256;
    std::string motionSha256;
    std::uint64_t renderSequence;
};

class AvatarFrameHub final {
public:
    core::Result<AvatarFrameReceipt> render(const AvatarFrameRequest& request);
    core::Result<void> swapRenderer(
        std::uint64_t generation,
        std::string artifactSha256,
        std::shared_ptr<IAvatarRenderer> renderer);
};

} // namespace creator::avatar

namespace creator::app {

class AvatarPreviewItem final : public VideoPreviewItem {
    Q_OBJECT
    Q_PROPERTY(qulonglong rendererGeneration READ rendererGeneration
               NOTIFY avatarRenderStateChanged)
    Q_PROPERTY(QString artifactSha256 READ artifactSha256
               NOTIFY avatarRenderStateChanged)
};

} // namespace creator::app
```

- [ ] **Step 1: Add failing single-renderer provenance tests**

```cpp
TEST(AvatarFrameHubTest, AllConsumersUseTheSameArtifactMotionAndRenderer) {
    auto hub = hubWithRealFixtureRenderer();
    std::vector<AvatarFrameReceipt> receipts;
    for (const auto consumer : allConsumers()) {
        receipts.push_back(hub.render(request(
            consumer,
            core::TimestampNs{core::DurationNs{100'000'000}},
            1280, 720)).value());
    }
    EXPECT_TRUE(allEqual(receipts, &AvatarFrameReceipt::rendererGeneration));
    EXPECT_TRUE(allEqual(receipts, &AvatarFrameReceipt::artifactSha256));
    EXPECT_TRUE(allEqual(receipts, &AvatarFrameReceipt::motionSha256));
    EXPECT_TRUE(allCpuReadbackHashesEqual(receipts));
}

TEST(AvatarPreviewItemTest, ShowsOnlyMailboxFramesFromActiveGeneration) {
    AvatarPreviewItem item;
    pushFrame(item, realRenderedFrame(7), 7);
    renderOneQtQuickFrame(item);
    EXPECT_TRUE(item.frameVisible());
    EXPECT_EQ(item.rendererGeneration(), 7U);
    pushFrame(item, realRenderedFrame(6), 6);
    renderOneQtQuickFrame(item);
    EXPECT_EQ(item.rendererGeneration(), 7U);
}
```

- [ ] **Step 2: Run and prove frame-hub tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarFrameHubTest|AvatarPreviewItemTest" --output-on-failure
```

Expected: frame hub, producer, and preview item are absent.

- [ ] **Step 3: Implement timestamped latest-frame fanout**

`AvatarFrameHub` owns one active renderer generation behind a short critical
section and renders on the render worker. It samples one `AvatarMotionFusion`
timeline, retargets once per requested timestamp, and caches equivalent
`(generation, timestamp, resolution, quality)` renders for fanout. Every receipt
contains artifact/motion provenance. A swap occurs only between frames after
the candidate renderer has produced one valid transparent validation frame.

`AvatarPreviewProducer` requests the workspace size/quality at display cadence
and publishes the newest `media::VideoFrame` to the existing bounded
`LatestVideoFrameMailbox`. `AvatarPreviewItem` reuses the established
`VideoPreviewItem` native D3D11/IOSurface path, retains the exact platform
handle displayed by the render node, and drops prior generations. CPU fallback
sets `rendererStatus` to `cpu-fallback`; it is not reported as zero-copy.

Do not add any QML image source as the ready-state content. Empty/loading/error
overlays may contain text and progress only; the avatar surface becomes visible
from a valid mailbox frame.

- [ ] **Step 4: Run provenance, swap, GPU retention, and visual tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarFrameHubTest|AvatarPreviewItemTest|VideoPreviewItemTest|AvatarRenderPipelineTest" --output-on-failure
```

Expected: all consumers share provenance and equivalent readback pixels,
generation swaps are atomic, GPU handles live through render, and preview never
displays a stale generation.

- [ ] **Step 5: Commit the single real render fanout**

```powershell
git add src/avatar/AvatarFrameHub.h src/avatar/AvatarFrameHub.cpp src/app/AvatarPreviewProducer.h src/app/AvatarPreviewProducer.cpp src/app/AvatarPreviewItem.h src/app/AvatarPreviewItem.cpp tests/avatar/AvatarFrameHubTest.cpp tests/app/AvatarPreviewItemTest.cpp src/avatar/AvatarRenderPipeline.h src/avatar/AvatarRenderPipeline.cpp src/app/VideoPreviewItem.h src/app/VideoPreviewItem.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): fan one renderer into every frame consumer"
```

## Task 4: Build the adaptive editing/tracking workspace and Android shell

**Files:**
- Create: `qml/AvatarPage.qml`
- Create: `qml/avatar/AvatarTopBar.qml`
- Create: `qml/avatar/AvatarCatalogPanel.qml`
- Create: `qml/avatar/AvatarPreviewPanel.qml`
- Create: `qml/avatar/AvatarInspectorPanel.qml`
- Create: `qml/avatar/AvatarStatusBar.qml`
- Create: `qml/avatar/AvatarRightsDialog.qml`
- Create: `qml/avatar/AvatarMissingAssetsDialog.qml`
- Create: `qml/avatar/AvatarCalibrationDialog.qml`
- Create: `qml/avatar/AvatarMobileSheet.qml`
- Create: `src/app/AvatarTrackingController.h`
- Create: `src/app/AvatarTrackingController.cpp`
- Create: `src/capture/android/QtAndroidCameraCaptureSource.h`
- Create: `src/capture/android/QtAndroidCameraCaptureSource.cpp`
- Create: `src/capture/android/QtAndroidAudioCaptureSource.h`
- Create: `src/capture/android/QtAndroidAudioCaptureSource.cpp`
- Create: `android/AndroidManifest.xml`
- Create: `android/res/xml/file_paths.xml`
- Create: `tests/qml/AvatarPageTest.cpp`
- Create: `tests/qml/AvatarAccessibilityTest.cpp`
- Modify: `qml/Main.qml`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/capture/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `translations/creatorstudio_ko.ts`
- Create: `translations/creatorstudio_en.ts`

**Interfaces:**
- Adds `Avatar` to `Main.qml` navigation and stack.
- Registers `AvatarPreviewItem` as `CreatorStudio.Native 1.0`.
- Adds root context properties `avatarEditorController`,
  and `avatarTrackingController`. Task 6 adds `avatarOutputController` together
  with the first export UI, so this task contains no disabled export control.
- Adds `android-debug` and `android-release` CMake presets targeting arm64-v8a.

- [ ] **Step 1: Add failing QML topology and interaction tests**

```cpp
TEST(AvatarPageTest, DesktopExposesAllWorkspaceRegionsAndRealPreview) {
    auto page = loadAvatarPage(1440, 900);
    EXPECT_TRUE(findItem(page, "avatarTopBar"));
    EXPECT_TRUE(findItem(page, "avatarCatalogPanel"));
    EXPECT_TRUE(findItem(page, "avatarPreviewItem"));
    EXPECT_TRUE(findItem(page, "avatarInspectorPanel"));
    EXPECT_TRUE(findItem(page, "avatarStatusBar"));
    EXPECT_EQ(typeName(findItem(page, "avatarPreviewItem")), "AvatarPreviewItem");
}

TEST(AvatarPageTest, MobileRetainsEditorTrackingAndRightsOperationsInSheets) {
    auto page = loadAvatarPage(412, 915);
    EXPECT_TRUE(invokeAndObserve(page, "openCatalog", catalogModelVisible()));
    EXPECT_TRUE(invokeAndObserve(page, "openInspector", inspectorModelVisible()));
    EXPECT_TRUE(invokeAndObserve(page, "openCalibration", calibrationVisible()));
    EXPECT_TRUE(invokeAndObserve(page, "openRights", rightsVisible()));
}

TEST(AvatarPageTest, EveryEditorControlCallsARealControllerMutation) {
    auto page = loadAvatarPageWithSpyController();
    interactWithEveryDeclaredControl(page);
    EXPECT_EQ(unconnectedInteractiveObjectNames(page), std::vector<QString>{});
}
```

- [ ] **Step 2: Run and prove workspace tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_qml_tests
ctest --test-dir build/windows-debug -R AvatarPageTest --output-on-failure
```

Expected: Avatar navigation, page, controls, and Android shell are absent.

- [ ] **Step 3: Implement desktop and mobile layouts from shared models**

Desktop layout:

- top bar: new/open/save/save-as, undo/redo, 2D/3D, family, randomize, compare;
- left: normalized search, family/slot/theme/rights filters, favorites/recent,
  real signed thumbnails, install state, compatibility reasons;
- center: `AvatarPreviewItem`, transparent/light/dark/chroma backgrounds, 3D
  360-degree orbit, bounded authored 2D angle test, broadcast-size inset;
- right: generated face/body/style/hair/animal/outfit/material/motion groups,
  slot locks, palette save/apply, precise collision/rights issues;
- bottom: camera/provider, calibration, expression test, subsystem confidence,
  frame/inference/render/memory/quality stats, rights summary.

At widths below 720 device-independent pixels, the preview remains central and
the catalog, inspector, tracking, performance, and rights surfaces move into
full-screen pages/bottom sheets backed by the same controller/models. Controls
are not duplicated with separate state.

`AvatarTrackingController` starts/stops the real provider and audio fallback,
requests camera/microphone permissions, exposes subsystem state/confidence and
calibration progress, and routes frames into `AvatarMotionFusion`.

On Android, Qt Multimedia adapters convert `QVideoFrame`/`QAudioBuffer` into the
existing `media::VideoFrame`/`AudioBlock` contracts, preserve timestamps,
bounded latest-frame queues, orientation, mirror state, and lifecycle pause.
The manifest declares camera and microphone permissions and no Internet
permission for tracking. Android file open/save uses the system document
provider and copies content through a bounded application service, not raw
QML paths.

All actions have shortcuts where a keyboard exists, accessible roles/names,
visible focus, Korean/English strings, 44-dp mobile targets, and no color-only
state indication.

- [ ] **Step 4: Run QML, translation, desktop build, and Android configure**

Run:

```powershell
cmake --build --preset windows-debug --target cs_qml_tests creator-studio
ctest --test-dir build/windows-debug -R "AvatarPageTest|AvatarEditorControllerTest|AvatarAccessibilityTest" --output-on-failure
cmake --preset android-debug
cmake --build --preset android-debug --target creator-studio
```

Expected: desktop and mobile topologies expose all editing, tracking, and rights
operations implemented in this task; controls mutate real session state,
translations are complete, and Android arm64 packages with camera/mic adapters
and permissions.

- [ ] **Step 5: Commit the adaptive production workspace**

```powershell
git add qml/AvatarPage.qml qml/avatar/AvatarTopBar.qml qml/avatar/AvatarCatalogPanel.qml qml/avatar/AvatarPreviewPanel.qml qml/avatar/AvatarInspectorPanel.qml qml/avatar/AvatarStatusBar.qml qml/avatar/AvatarRightsDialog.qml qml/avatar/AvatarMissingAssetsDialog.qml qml/avatar/AvatarCalibrationDialog.qml qml/avatar/AvatarMobileSheet.qml src/app/AvatarTrackingController.h src/app/AvatarTrackingController.cpp src/capture/android/QtAndroidCameraCaptureSource.h src/capture/android/QtAndroidCameraCaptureSource.cpp src/capture/android/QtAndroidAudioCaptureSource.h src/capture/android/QtAndroidAudioCaptureSource.cpp android/AndroidManifest.xml android/res/xml/file_paths.xml tests/qml/AvatarPageTest.cpp tests/qml/AvatarAccessibilityTest.cpp qml/Main.qml src/main.cpp CMakeLists.txt CMakePresets.json src/app/CMakeLists.txt src/capture/CMakeLists.txt tests/CMakeLists.txt translations/creatorstudio_ko.ts translations/creatorstudio_en.ts
git commit -m "feat(avatar): add complete adaptive avatar workspace"
```

## Task 5: Persist, autosave, recover, and repair avatar sessions

**Files:**
- Create: `src/app/AvatarAutosaveCoordinator.h`
- Create: `src/app/AvatarAutosaveCoordinator.cpp`
- Create: `src/app/AvatarRecoveryController.h`
- Create: `src/app/AvatarRecoveryController.cpp`
- Create: `tests/app/AvatarAutosaveCoordinatorTest.cpp`
- Create: `tests/app/AvatarRecoveryControllerTest.cpp`
- Modify: `src/project_store/IAvatarSpecStore.h`
- Modify: `src/project_store/AvatarSpecFileStore.h`
- Modify: `src/project_store/AvatarSpecFileStore.cpp`
- Modify: `src/project_store/JsonProjectStore.h`
- Modify: `src/project_store/JsonProjectStore.cpp`
- Modify: `src/project_store/ProjectPackageStore.h`
- Modify: `src/project_store/ProjectPackageStore.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `src/app/ProjectController.h`
- Modify: `src/app/ProjectController.cpp`
- Modify: `qml/RecoveryPage.qml`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::app {

struct AvatarRecoveryCandidate {
    std::string avatarId;
    std::uint64_t revision;
    core::Utc savedAt;
    std::string specSha256;
    std::string artifactSha256;
    std::vector<avatar::MissingAvatarAsset> missingAssets;
};

class AvatarAutosaveCoordinator final : public QObject {
    Q_OBJECT
public:
    void schedule(avatar::AvatarEditorSnapshot snapshot);
    void flush();
};

class AvatarRecoveryController final : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void recover(QString avatarId, qulonglong revision);
    Q_INVOKABLE void discardAutosave(QString avatarId, qulonglong revision);
    Q_INVOKABLE void installMissing(QString assetId, QString version);
    Q_INVOKABLE void removeMissingSlot(QString slot);
    Q_INVOKABLE void chooseReplacement(QString slot, QString assetId,
                                       QString version);
};

} // namespace creator::app
```

- [ ] **Step 1: Add failing autosave/crash/missing-asset tests**

```cpp
TEST(AvatarAutosaveCoordinatorTest, DebouncesAndAtomicallyRetainsLastGood) {
    scheduleRapidRevisions(10);
    advanceEventLoop(core::DurationNs{749'000'000});
    EXPECT_EQ(storeWrites(), 0);
    advanceEventLoop(core::DurationNs{1'000'000});
    ASSERT_EQ(storeWrites(), 1);
    EXPECT_EQ(savedRevision(), 10U);
    simulateWriteFailureAtRename();
    EXPECT_EQ(loadLastGood().revision, 10U);
}

TEST(AvatarRecoveryControllerTest, NeverSubstitutesMissingAssets) {
    openRecoveryWithMissing("core.hair.layered-bob", "2.1.0", knownHash());
    EXPECT_EQ(recoveredSpec().slots().at(AvatarSlot::HairFront).assetId,
              AvatarAssetId::from("core.hair.layered-bob").value());
    EXPECT_TRUE(previewBlockedForMissingSlot(AvatarSlot::HairFront));
    EXPECT_TRUE(actionsAreExactly({"install", "remove-slot", "replace"}));
}
```

- [ ] **Step 2: Run and prove persistence coordinator tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarAutosaveCoordinatorTest|AvatarRecoveryControllerTest" --output-on-failure
```

Expected: autosave/recovery coordination is absent.

- [ ] **Step 3: Implement revisioned transactional storage**

For each avatar, store:

```text
avatars/<avatar-id>/
  current.json
  last-good.json
  compiled/<artifact-sha256>/...
  rights/<rights-snapshot-sha256>.json
  history/commands.ndjson
autosave/avatars/<avatar-id>/<revision>.json
```

`current.json` contains canonical `AvatarSpec`, exact asset ID/version/hash,
rig/profile versions, active artifact hash, rights snapshot hash, calibration
reference, and schema version. Saving writes files in the same package
filesystem, fsyncs, verifies readback/hash, atomically renames, then updates
project `updatedAt`. `last-good.json` advances only after the compiled artifact
opens and the current snapshot commits.

Autosave waits 750 ms after the last semantic edit, retains the newest three
verified revisions, never deletes `last-good`, and retries only on an explicit
new edit or user action after failure. Project open compares current/autosave
revision and surfaces recovery before compiling.

Missing assets preserve the exact reference and block affected compilation.
Install validates signed package/version/hash; remove is a reversible semantic
edit; replacement requires compatibility and rights confirmation. Schema
migrations operate on a copy, validate, write a new revision, and retain the
original until the migrated project opens.

- [ ] **Step 4: Run save, crash injection, migration, and recovery tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests cs_project_store_tests
ctest --test-dir build/windows-debug -R "AvatarAutosaveCoordinatorTest|AvatarRecoveryControllerTest|AvatarSpecFileStoreTest|JsonProjectStoreTest|ProjectPackageStoreTest" --output-on-failure
```

Expected: debounced saves are atomic, injected failures preserve last-good,
recovery selection is explicit, missing assets stay missing, and migrations are
rollback-safe.

- [ ] **Step 5: Commit avatar save and recovery**

```powershell
git add src/app/AvatarAutosaveCoordinator.h src/app/AvatarAutosaveCoordinator.cpp src/app/AvatarRecoveryController.h src/app/AvatarRecoveryController.cpp tests/app/AvatarAutosaveCoordinatorTest.cpp tests/app/AvatarRecoveryControllerTest.cpp src/project_store/IAvatarSpecStore.h src/project_store/AvatarSpecFileStore.h src/project_store/AvatarSpecFileStore.cpp src/project_store/JsonProjectStore.h src/project_store/JsonProjectStore.cpp src/project_store/ProjectPackageStore.h src/project_store/ProjectPackageStore.cpp src/project_store/CMakeLists.txt src/app/ProjectController.h src/app/ProjectController.cpp qml/RecoveryPage.qml src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): persist and recover verified avatar revisions"
```

## Task 6: Import and export real models under current rights

**Files:**
- Create: `src/avatar_2d_adapter/Inochi2dModelExporter.h`
- Create: `src/avatar_2d_adapter/Inochi2dModelExporter.cpp`
- Create: `src/app/AvatarOutputController.h`
- Create: `src/app/AvatarOutputController.cpp`
- Create: `qml/avatar/AvatarExportDialog.qml`
- Create: `tests/avatar_2d_adapter/Inochi2dModelExporterTest.cpp`
- Create: `tests/app/AvatarOutputControllerTest.cpp`
- Modify: `src/avatar_2d_adapter/CMakeLists.txt`
- Modify: `src/avatar_3d_adapter/Avatar3dExporter.h`
- Modify: `src/avatar_3d_adapter/Avatar3dExporter.cpp`
- Modify: `qml/AvatarPage.qml`
- Modify: `qml/avatar/AvatarMobileSheet.qml`
- Modify: `src/main.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar2d {

enum class Inochi2dExportFormat { InxEditable, InpPuppet };

struct Inochi2dExportRequest {
    avatar::AvatarSpec spec;
    std::filesystem::path destination;
    bool replaceExisting{false};
};

struct Inochi2dExportReceipt {
    std::filesystem::path path;
    std::string outputSha256;
    avatar::AvatarRightsDecision rightsDecision;
    std::string rightsSnapshotSha256;
};

class Inochi2dModelExporter final {
public:
    core::Result<Inochi2dExportReceipt> exportModel(
        const Inochi2dExportRequest& request,
        const avatar::CompiledAvatarArtifact& artifact,
        Inochi2dExportFormat format);
};

} // namespace creator::avatar2d
```

- [ ] **Step 1: Add failing round-trip/rights/format tests**

```cpp
TEST(Inochi2dModelExporterTest, WritesRuntimeLoadableInxAndInp) {
    for (const auto format : {Inochi2dExportFormat::InxEditable,
                              Inochi2dExportFormat::InpPuppet}) {
        const auto receipt = exporter().exportModel(
            allowedRequest(format), compiledFixture(), format);
        ASSERT_TRUE(receipt.hasValue()) << receipt.error().message();
        EXPECT_TRUE(InpContainer::parse(readBytes(receipt.value().path),
                                        productionLimits()).hasValue());
        EXPECT_TRUE(verifiedInochiRuntimeLoads(receipt.value().path));
    }
}

TEST(AvatarOutputControllerTest, ShowsExactRestrictingAssetsAndCreatesNoFile) {
    installCurrentRightsThatBlockModelExport("core.outfit.broadcast-only");
    controller().exportModel(exportUrl());
    EXPECT_FALSE(fileExists(exportPath()));
    EXPECT_TRUE(rightsModelContainsRestrictor("core.outfit.broadcast-only"));
}

TEST(AvatarOutputControllerTest, OffersFormatsTruthfulToRepresentation) {
    EXPECT_EQ(formatsFor(quadruped3d()),
              QStringList({"gltf-2.0-creator-rig"}));
    EXPECT_EQ(formatsFor(humanoid3d()), QStringList({"vrm-1.0"}));
    EXPECT_EQ(formatsFor(inochi2d()), QStringList({"inx", "inp"}));
}
```

- [ ] **Step 2: Run and prove output tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests cs_app_tests
ctest --test-dir build/windows-debug -R "Inochi2dModelExporterTest|AvatarOutputControllerTest" --output-on-failure
```

Expected: 2D exporter and output controller are absent.

- [ ] **Step 3: Implement strict import/export orchestration**

At click time the controller reloads the current spec/assets, re-verifies
package signatures/hashes, resolves rights with `UseKind::ModelExport`, and
delegates to
the representation exporter. Denial lists every restricting asset and creates
no temporary destination.

`InxEditable` retains valid Inochi edit metadata, module provenance, mesh,
deformer, parameter, texture, mask, physics, and Creator Studio rights snapshot
extension data. `InpPuppet` writes the actual runtime puppet and omits only
edit-only provenance. Both are big-endian `TRNSRTS\0` containers written by
`InpContainer`, reparsed, hash-verified, and loaded by the pinned Inochi runtime.
No file is renamed between extensions as a substitute for the required metadata.

3D delegates to `Avatar3dExporter`: VRM for validated humanoid-compatible rigs,
glTF plus Creator Studio rig metadata for non-humanoids. Every format writes to
a sibling temporary location, verifies, then atomically publishes.

Import accepts external `.inx`, `.inp`, `.vrm`, and `.glb`, validates bounded
formats, extracts embedded license metadata, asks for explicit rights
classification when evidence is missing, and installs a signed local package
without modifying the source. Unknown rights allow private preview only; they
cannot authorize commercial broadcast, bundling, derivatives, or export.

Add the first export dialog and `avatarOutputController` root context property
in this task. Desktop top bar and Android mobile sheet open the same
controller-backed dialog; format choices and enabled state come from the
controller. No disabled or disconnected export surface exists in Task 4.

- [ ] **Step 4: Run format round-trip, rights-race, and failure tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests cs_avatar_3d_adapter_tests cs_app_tests
ctest --test-dir build/windows-debug -R "Inochi2dModelExporterTest|Avatar3dExporterTest|AvatarOutputControllerTest" --output-on-failure
```

Expected: each truthful format round-trips through its real parser/runtime,
rights changes between dialog open and click block output, and partial files do
not remain.

- [ ] **Step 5: Commit real rights-checked model IO**

```powershell
git add src/avatar_2d_adapter/Inochi2dModelExporter.h src/avatar_2d_adapter/Inochi2dModelExporter.cpp src/app/AvatarOutputController.h src/app/AvatarOutputController.cpp qml/avatar/AvatarExportDialog.qml tests/avatar_2d_adapter/Inochi2dModelExporterTest.cpp tests/app/AvatarOutputControllerTest.cpp src/avatar_2d_adapter/CMakeLists.txt src/avatar_3d_adapter/Avatar3dExporter.h src/avatar_3d_adapter/Avatar3dExporter.cpp qml/AvatarPage.qml qml/avatar/AvatarMobileSheet.qml src/main.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): import and export truthful licensed models"
```

## Task 7: Register the active avatar as a real Studio and recording source

**Files:**
- Create: `src/app/AvatarSourceRegistry.h`
- Create: `src/app/AvatarSourceRegistry.cpp`
- Create: `tests/app/AvatarSourceRegistryTest.cpp`
- Modify: `src/capture/AvatarRenderCaptureSource.h`
- Modify: `src/capture/AvatarRenderCaptureSource.cpp`
- Modify: `src/app/ControllerLiveCaptureBindings.h`
- Modify: `src/app/ControllerLiveCaptureBindings.cpp`
- Modify: `src/app/StudioWorkflowController.h`
- Modify: `src/app/StudioWorkflowController.cpp`
- Modify: `src/app/StudioWorkflowTypes.h`
- Modify: `qml/StudioPage.qml`
- Modify: `src/main.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::app {

class AvatarSourceRegistry final {
public:
    core::Result<domain::SourceId> publish(
        avatar::AvatarId avatarId,
        std::shared_ptr<avatar::AvatarFrameHub> frameHub);
    core::Result<void> unpublish(avatar::AvatarId avatarId);
    std::vector<LiveCaptureSource> sources() const;
};

} // namespace creator::app
```

- [ ] **Step 1: Add failing Studio source and pixel-equivalence tests**

```cpp
TEST(AvatarSourceRegistryTest, PublishesOnlyReadyActiveAvatars) {
    auto registry = registryFixture();
    EXPECT_EQ(registry.publish(avatarId(), frameHubWithoutRenderer()).error().code(),
              core::ErrorCode::InvalidState);
    ASSERT_TRUE(registry.publish(avatarId(), readyFrameHub()).hasValue());
    EXPECT_EQ(registry.sources().size(), 1U);
}

TEST(AvatarSourceRegistryTest, StudioRecordingMatchesWorkspaceRenderer) {
    auto source = publishAndOpenSource();
    const auto preview = frameHub().render(
        request(AvatarFrameConsumer::WorkspacePreview,
                core::TimestampNs{core::DurationNs{500'000'000}},
                1920, 1080));
    const auto recorded = source.tickAt(
        core::TimestampNs{core::DurationNs{500'000'000}});
    ASSERT_TRUE(preview.hasValue());
    ASSERT_TRUE(recorded.hasValue());
    EXPECT_EQ(readbackHash(preview.value().frame),
              readbackHash(recorded.value()));
}
```

- [ ] **Step 2: Run and prove Studio integration fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
ctest --test-dir build/windows-debug -R AvatarSourceRegistryTest --output-on-failure
```

Expected: avatar registry and Studio avatar-source choice are absent.

- [ ] **Step 3: Implement a normal live source backed by `AvatarFrameHub`**

Publish a source only after the editor has a verified active renderer. The
source ID is stable per project/avatar. `AvatarRenderCaptureSource` requests
`AvatarFrameConsumer::Recording` frames from the hub at exact rational
timestamps and preserves alpha/pixel format/platform handle. It reports dropped
or CPU-fallback frames in `CaptureStats`.

Add published avatar sources to `ControllerLiveCaptureBindings` alongside
screen/camera sources. Studio source UI shows the actual avatar thumbnail,
generation, resolution, alpha, and readiness; selecting it creates a normal
scene source with existing transform/crop/opacity/z-order support. Recording
uses the established recorder and media package path. Edit playback reads the
recorded frames; live re-render is offered only as an explicit linked-avatar
mode with artifact/motion hashes pinned.

Changing an avatar during recording creates a pending generation but does not
replace the recorded source until a frame boundary and records a source-change
event containing both artifact hashes. Deleting/unpublishing an avatar already
used in a project preserves its saved spec/artifact reference.

- [ ] **Step 4: Run Studio workflow, recording, alpha, and change tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests cs_acceptance_tests
ctest --test-dir build/windows-debug -R "AvatarSourceRegistryTest|AvatarRenderCaptureSourceTest|StudioWorkflowControllerTest|R1StudioWorkflowAcceptanceTest" --output-on-failure
```

Expected: the ready avatar appears as a source, transformed recording retains
real pixels/alpha/timestamps, generation changes are recorded, and existing
Studio tests pass.

- [ ] **Step 5: Commit Studio and recording integration**

```powershell
git add src/app/AvatarSourceRegistry.h src/app/AvatarSourceRegistry.cpp tests/app/AvatarSourceRegistryTest.cpp src/capture/AvatarRenderCaptureSource.h src/capture/AvatarRenderCaptureSource.cpp src/app/ControllerLiveCaptureBindings.h src/app/ControllerLiveCaptureBindings.cpp src/app/StudioWorkflowController.h src/app/StudioWorkflowController.cpp src/app/StudioWorkflowTypes.h qml/StudioPage.qml src/main.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): publish active avatars to Studio recording"
```

## Task 8: Deliver verified alpha broadcast and universal chroma fallback

**Files:**
- Create: `src/avatar/AvatarBroadcastProtocol.h`
- Create: `src/avatar/AvatarBroadcastProtocol.cpp`
- Create: `src/avatar/IAvatarBroadcastSink.h`
- Create: `src/avatar/SharedAvatarBroadcastSink.h`
- Create: `src/avatar/SharedAvatarBroadcastSink.cpp`
- Create: `src/avatar/ChromaCollisionAnalyzer.h`
- Create: `src/avatar/ChromaCollisionAnalyzer.cpp`
- Create: `src/app/ChromaOutputWindow.h`
- Create: `src/app/ChromaOutputWindow.cpp`
- Create: `tools/obs-avatar-source/CMakeLists.txt`
- Create: `tools/obs-avatar-source/LICENSE`
- Create: `tools/obs-avatar-source/README.md`
- Create: `tools/obs-avatar-source/src/obs-avatar-source.c`
- Create: `tools/obs-avatar-source/src/avatar-ipc-reader.h`
- Create: `tools/obs-avatar-source/src/avatar-ipc-reader.c`
- Create: `tools/obs-avatar-source/tests/avatar-ipc-reader-test.cpp`
- Create: `scripts/bootstrap_obs_sdk.ps1`
- Create: `scripts/build_obs_avatar_source.ps1`
- Create: `scripts/verify_avatar_output.ps1`
- Create: `tests/avatar/AvatarBroadcastProtocolTest.cpp`
- Create: `tests/avatar/ChromaCollisionAnalyzerTest.cpp`
- Modify: `src/app/AvatarOutputController.h`
- Modify: `src/app/AvatarOutputController.cpp`
- Modify: `src/main.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `legal/OSS_BOM.csv`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class AvatarBroadcastBackend {
    D3d11SharedTexture, IOSurface, CpuRgbaRing, ChromaWindow
};

struct AvatarBroadcastDescriptor {
    std::array<char, 8> magic{'C','S','A','V','F','R','M','\0'};
    std::uint32_t protocolVersion{1};
    AvatarBroadcastBackend backend;
    std::uint64_t generation;
    std::uint64_t sequence;
    std::int64_t timestampNs;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t rowBytes;
    media::PixelFormat pixelFormat;
    bool premultipliedAlpha;
    std::array<std::byte, 32> frameSha256;
};

class IAvatarBroadcastSink {
public:
    virtual ~IAvatarBroadcastSink() = default;
    virtual core::Result<void> start(const AvatarBroadcastOptions& options) = 0;
    virtual core::Result<void> publish(const AvatarFrameReceipt& frame) = 0;
    virtual core::Result<void> stop() = 0;
    virtual AvatarBroadcastStatus status() const = 0;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing protocol, alpha-handshake, and chroma tests**

```cpp
TEST(AvatarBroadcastProtocolTest, RejectsWrongVersionStaleGenerationAndBadHash) {
    EXPECT_EQ(decodeDescriptor(withVersion(2)).error().code(),
              core::ErrorCode::UnsupportedVersion);
    auto sink = connectedSinkAtGeneration(8);
    EXPECT_EQ(sink.publish(frameAtGeneration(7)).error().code(),
              core::ErrorCode::InvalidState);
    EXPECT_EQ(readFrame(withChangedPixel()).error().code(),
              core::ErrorCode::IoFailure);
}

TEST(AvatarOutputControllerTest, ClaimsAlphaOnlyAfterVerifiedConsumerFrame) {
    controller().startBroadcast();
    EXPECT_FALSE(controller().alphaOutputActive());
    simulateConsumerHandshakeAndVerifiedFrame();
    EXPECT_TRUE(controller().alphaOutputActive());
    simulateConsumerDisconnect();
    EXPECT_FALSE(controller().alphaOutputActive());
    EXPECT_TRUE(controller().chromaAvailable());
}

TEST(ChromaCollisionAnalyzerTest, ChoosesLowestCollisionColorAndWarns) {
    const auto result = analyzer().analyze(realAvatarFrame(),
        {rgb("#00FF00"), rgb("#0000FF"), rgb("#FF00FF")});
    EXPECT_EQ(result.selected, rgb("#0000FF"));
    EXPECT_LT(result.collisionRatio, 0.01F);
}
```

- [ ] **Step 2: Run and prove broadcast tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarBroadcastProtocolTest|ChromaCollisionAnalyzerTest|AvatarOutputControllerTest" --output-on-failure
```

Expected: protocol, sinks, plugin, and chroma output are absent.

- [ ] **Step 3: Implement isolated verified broadcast paths**

Closed-core producer:

- Windows shares a D3D11 texture created with an OS shareable handle and keyed
  synchronization; macOS publishes an IOSurface reference with generation and
  lifetime handshake; both fall back to a bounded triple-buffered premultiplied
  BGRA ring when verified native import is unavailable.
- IPC control uses a per-user random 256-bit session token, owner-only
  permissions, fixed-size little-endian descriptors, checked arithmetic,
  sequence/generation/timestamp, heartbeat, and frame hash for the initial
  validation/readback. Unauthenticated or wrong-version consumers receive no
  handle.
- The sink publishes only frames received from `AvatarFrameHub` with broadcast
  provenance. It keeps at most three frames, drops oldest, and never blocks the
  render loop waiting for OBS.

Isolated OBS plugin:

- `tools/obs-avatar-source` is licensed GPL-2.0-or-later, has its own build,
  notices, source distribution, and includes OBS headers only inside that tree.
  It links to OBS Studio 32.2.0 plugin SDK; `bootstrap_obs_sdk.ps1` verifies the
  official source archive SHA-256
  `a26a5da53964a8c38741c613f14f93cc37d95354e5225a07b6618152cdfcec1c`.
- It registers one “Creator Studio Avatar” source, imports D3D11/IOSurface or
  CPU BGRA, preserves premultiplied alpha and timestamps, releases old
  generations, reports status, and renders a transparent OBS texture.
- Closed-core CMake never adds this subdirectory, includes its headers, links
  its targets, or packages it into the proprietary executable. Distribution
  places plugin binary/source/license in a separately identified optional
  package and fulfills GPL source obligations.

Chroma fallback:

- `ChromaOutputWindow` is borderless, always-on-top optional, output-resolution
  exact, unscaled, and renders `AvatarFrameHub` frames over the selected opaque
  color through the same avatar renderer.
- Analyzer samples visible avatar RGB in CIE Lab, scores green/blue/magenta
  candidates within a configurable key distance, reports collision ratio and
  conflicting regions, and requires confirmation above 1%.
- Android exposes the chroma surface through a full-screen activity/shareable
  display because desktop GPU IPC and OBS plugins do not apply there.

- [ ] **Step 4: Build and verify core output and the separate plugin**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests cs_app_tests
ctest --test-dir build/windows-debug -R "AvatarBroadcastProtocolTest|ChromaCollisionAnalyzerTest|AvatarOutputControllerTest" --output-on-failure
pwsh -NoProfile -File scripts/bootstrap_obs_sdk.ps1 -Version 32.2.0
pwsh -NoProfile -File scripts/build_obs_avatar_source.ps1 -Preset windows-release
pwsh -NoProfile -File scripts/verify_avatar_output.ps1 -CorePreset windows-debug -ObsPlugin build/obs-avatar-source
```

Expected: core alpha IPC passes authenticated cross-process pixel/hash tests,
the separately built plugin displays matching transparent pixels in OBS 32.2.0,
consumer loss exposes chroma fallback, and the closed-core link map contains no
OBS/GPL symbols or libraries.

- [ ] **Step 5: Commit broadcast output with license isolation**

```powershell
git add src/avatar/AvatarBroadcastProtocol.h src/avatar/AvatarBroadcastProtocol.cpp src/avatar/IAvatarBroadcastSink.h src/avatar/SharedAvatarBroadcastSink.h src/avatar/SharedAvatarBroadcastSink.cpp src/avatar/ChromaCollisionAnalyzer.h src/avatar/ChromaCollisionAnalyzer.cpp src/app/ChromaOutputWindow.h src/app/ChromaOutputWindow.cpp tools/obs-avatar-source/CMakeLists.txt tools/obs-avatar-source/LICENSE tools/obs-avatar-source/README.md tools/obs-avatar-source/src/obs-avatar-source.c tools/obs-avatar-source/src/avatar-ipc-reader.h tools/obs-avatar-source/src/avatar-ipc-reader.c tools/obs-avatar-source/tests/avatar-ipc-reader-test.cpp scripts/bootstrap_obs_sdk.ps1 scripts/build_obs_avatar_source.ps1 scripts/verify_avatar_output.ps1 tests/avatar/AvatarBroadcastProtocolTest.cpp tests/avatar/ChromaCollisionAnalyzerTest.cpp src/app/AvatarOutputController.h src/app/AvatarOutputController.cpp src/main.cpp src/avatar/CMakeLists.txt src/app/CMakeLists.txt tests/CMakeLists.txt legal/OSS_BOM.csv
git commit -m "feat(avatar): add isolated alpha broadcast and chroma output"
```

## Task 9: Qualify the connected workspace-to-output workflow

**Files:**
- Create: `tests/acceptance/AvatarWorkspaceOutputAcceptanceTest.cpp`
- Create: `tests/fixtures/avatar-workspace/README.md`
- Create: `scripts/audit_avatar_qml_connections.ps1`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`

**Interfaces:**
- Consumes: editor session/controller/QML, real compilers/renderers, tracking,
  storage, Studio source, exporters, broadcast sinks.
- Produces: connected-feature release gate.

- [ ] **Step 1: Add the failing complete user-journey test**

```cpp
TEST(AvatarWorkspaceOutputAcceptanceTest, EditTrackSaveReopenRecordAndBroadcastMatch) {
    auto app = launchWithSignedProductionFixtures();
    app.openAvatarWorkspace();
    app.create("inochi2d", "kemonomimi");
    app.selectAsset(AvatarSlot::HairFront, "core.hair.layered-bob");
    app.setColor("hair.base", "#7B61FF");
    app.selectAsset(AvatarSlot::Outerwear, "core.outfit.modern-jacket");
    app.calibrateWith(consentTrackingSequence());
    app.playTrackingSequence();
    const auto workspace = app.captureWorkspaceFrame(
        core::TimestampNs{core::DurationNs{1'000'000'000}});
    app.save();
    app.closeAndReopenProject();
    const auto reopened = app.captureWorkspaceFrame(
        core::TimestampNs{core::DurationNs{1'000'000'000}});
    app.addActiveAvatarToStudio();
    const auto recorded = app.recordOneFrameAt(
        core::TimestampNs{core::DurationNs{1'000'000'000}});
    const auto broadcast = app.readBroadcastFrameAt(
        core::TimestampNs{core::DurationNs{1'000'000'000}});
    EXPECT_EQ(workspace.pixelHash, reopened.pixelHash);
    EXPECT_EQ(workspace.pixelHash, recorded.pixelHash);
    EXPECT_EQ(workspace.pixelHash, broadcast.pixelHash);
    EXPECT_EQ(workspace.artifactSha256, broadcast.artifactSha256);
}

TEST(AvatarWorkspaceOutputAcceptanceTest, EveryAdvertisedPathRejectsFakeSuccess) {
    auto app = launchWithCompilerThatFailsAfterEdit();
    app.makeEdit();
    EXPECT_TRUE(app.previousRealPreviewRemains());
    EXPECT_FALSE(app.statusSaysSavedOrReadyForFailedGeneration());
    EXPECT_FALSE(app.canPublishFailedGeneration());
    EXPECT_EQ(app.unconnectedControlObjectNames(), std::vector<std::string>{});
}
```

- [ ] **Step 2: Run and prove the connected acceptance gate fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_workspace_output_acceptance_tests
ctest --test-dir build/windows-debug -R AvatarWorkspaceOutputAcceptanceTest --output-on-failure
```

Expected: the complete connected harness and audit script are absent.

- [ ] **Step 3: Add deterministic GUI/output fixtures and connection audit**

Use signed final-quality 2D humanoid/animal and 3D humanoid/animal fixtures,
fixed motion recordings, fixed project time, isolated settings/project roots,
offscreen Qt Quick rendering, real runtime libraries, and CPU readback only for
hash comparison. The test may not replace compilers/renderers in the success
journey.

`audit_avatar_qml_connections.ps1` parses all `qml/avatar/*.qml` interactive
objects, requires stable `objectName`, accessible name, enabled rule, and
controller action or model-bound edit delegate. It cross-checks every design
category and operation:
face, body, style, hair, animal, outfit, material, motion, search, filters,
favorites, recent, locks, randomize, palette, compare, undo/redo, save/recovery,
calibration, performance, rights, import/export, Studio, recording, alpha
broadcast, and chroma.

The acceptance journey runs once per representation/family fixture and once at
1440x900 and 412x915 topology. It injects missing assets, rights denial, compile
failure, autosave failure, GPU recreation, camera denial, provider loss, and
broadcast disconnect, verifying the documented recovery path while the last
good avatar remains available where safe.

- [ ] **Step 4: Run all connected gates**

Run:

```powershell
pwsh -NoProfile -File scripts/audit_avatar_qml_connections.ps1
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug -R "AvatarWorkspaceOutputAcceptanceTest|AvatarPageTest|AvatarSourceRegistryTest|AvatarOutputControllerTest|AvatarBroadcastProtocolTest" --output-on-failure
ctest --test-dir build/windows-debug --output-on-failure
git diff --check
```

Expected: every visible control is connected, real edits survive reopen, motion
changes real pixels, all output consumers match renderer provenance/pixels, and
injected failures never produce a false ready/saved/published state.

- [ ] **Step 5: Commit the connected workflow gate**

```powershell
git add tests/acceptance/AvatarWorkspaceOutputAcceptanceTest.cpp tests/fixtures/avatar-workspace/README.md scripts/audit_avatar_qml_connections.ps1 tests/CMakeLists.txt README.md
git commit -m "test(avatar): qualify connected workspace and output"
```

## Plan Completion Gate

- Desktop and Android expose the same complete customization, calibration,
  rights, save, export, Studio, and output operations through adaptive layouts.
- Every edit is a validated reversible command that compiles off-thread and
  atomically swaps only a verified real renderer generation.
- Autosave/recovery preserves exact assets, hashes, rights, and last-good
  artifacts; missing content is never silently substituted.
- Preview, Studio, recording, edit playback, and broadcast receipts share the
  same artifact, motion, renderer generation, timestamps, and equivalent
  pixels.
- `.inx`/`.inp`, VRM, and non-humanoid glTF exports are real round-tripping
  formats and are blocked when current transitive rights deny model export.
- OBS alpha output works through an authenticated separately licensed plugin;
  the closed core has no GPL link, and every platform has a collision-checked
  chroma fallback.
- The connected acceptance journey and QML audit make static previews,
  disconnected controls, false ready states, and alternate fake render paths
  release-blocking failures.
