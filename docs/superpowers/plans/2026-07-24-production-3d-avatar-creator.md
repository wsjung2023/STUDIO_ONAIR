# Production 3D Avatar Creator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile commercially cleared modular meshes into real cel-shaded 3D avatars, render humanoid VRM 1.0 and non-humanoid glTF rigs through one production pipeline, and export only formats and rights combinations that are truthful and permitted.

**Architecture:** A Qt-free 3D adapter validates bounded GLB containers, separates VRM 1.0 humanoid semantics from the Creator Studio non-humanoid rig extension, verifies modular skeleton compatibility, and composes deterministic render artifacts. Filament owns the GPU renderer and MToon-compatible materials; meshoptimizer owns deterministic index/vertex optimization and LOD generation. Humanoid export writes valid VRM 1.0 metadata, while mascot, quadruped, and avian export writes glTF 2.0 plus a Creator Studio rig sidecar. Every export re-evaluates the current catalog rights instead of trusting a stale UI decision.

**Tech Stack:** C++20, Filament v1.74.0 (`029f2af886174117999d118ea3dfdefafac364d6`, Apache-2.0), meshoptimizer v1.2 (`9d9890c73011d75920af614485296d1e03e95448`, source archive SHA-256 `e40f71b809cdf3361b9a4def85fd44534e8733ce29d4b943c145b76859e4c2b4`, MIT), glTF 2.0, VRM 1.0, MToon 1.0, nlohmann/json 3.11.3, GoogleTest 1.15.2.

## Global Constraints

- Requires completion of `2026-07-24-avatar-platform-foundation.md`.
- Uses `AvatarCompileRequest`, `CompiledAvatarArtifact`, and `IAvatarCompiler` from `2026-07-24-production-2d-avatar-creator.md`; the common contract must not fork into a second UI-only compiler API.
- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- `cs_avatar_3d_adapter` remains Qt-free. Qt, QML, file dialogs, and UI state stay outside it.
- `AvatarRepresentation::Vrm1` is valid only for a VRM 1.0 humanoid. `AvatarRepresentation::GltfRig` is used for non-humanoid mascot, quadruped, and avian rigs.
- A non-humanoid file is never relabeled as VRM, and a failed VRM humanoid validation never silently falls back to a “VRM-like” export.
- GLB parsing checks all 64-bit length arithmetic before allocation and applies documented byte, node, primitive, joint, morph, texture, animation, and JSON-depth limits.
- Module composition is allowed only when rig family, master-rig version, skeleton fingerprint, coordinate system, units, bind poses, and declared attachment slot are compatible.
- The same Filament renderer and compiled artifact feed editor preview, recording, and broadcast output.
- All cel-shading material packages are built by the matching Filament `matc`; generated material binaries are never accepted from an unverified asset package.
- Compiler output is deterministic for the same `AvatarSpec`, asset hashes, compiler version, renderer ABI, and quality tier.
- Raw model export is blocked unless `AvatarLicenseResolver::resolve` with
  `UseKind::ModelExport` allows every transitive asset and the output format
  supports the selected rig family.
- Filament and meshoptimizer licenses cover the software only; they do not grant rights to bundled VRM, mesh, texture, animation, or material assets.

---

## File Structure

```text
cmake/
  FindFilament.cmake
scripts/
  bootstrap_filament.ps1
  bootstrap_meshoptimizer.ps1
  build_avatar_materials.ps1
src/avatar_3d_adapter/
  CMakeLists.txt
  GlbDocument.h/.cpp
  Vrm1Document.h/.cpp
  Vrm1MetaRights.h/.cpp
  CreatorStudioRigDocument.h/.cpp
  Avatar3dModuleManifest.h/.cpp
  Avatar3dModuleManifestCodec.h/.cpp
  Avatar3dModuleComposer.h/.cpp
  Avatar3dAvatarCompiler.h/.cpp
  Avatar3dCompileCache.h/.cpp
  FilamentAvatarRenderer.h/.cpp
  FilamentAvatarRendererFactory.h/.cpp
  FilamentMaterialLibrary.h/.cpp
  Avatar3dExporter.h/.cpp
materials/avatar/
  mtoon_opaque.mat
  mtoon_transparent.mat
  mtoon_outline.mat
schemas/
  avatar-3d-module.schema.json
  creator-studio-rig.schema.json
tests/avatar_3d_adapter/
  GlbDocumentTest.cpp
  Vrm1DocumentTest.cpp
  CreatorStudioRigDocumentTest.cpp
  Avatar3dModuleManifestTest.cpp
  Avatar3dModuleComposerTest.cpp
  Avatar3dAvatarCompilerTest.cpp
  FilamentAvatarRendererTest.cpp
  Avatar3dExporterTest.cpp
tests/scripts/
  Avatar3dDependencyPinPolicyTest.ps1
tests/acceptance/
  Production3dAvatarAcceptanceTest.cpp
```

## Task 1: Pin reproducible Filament and meshoptimizer toolchains

**Files:**
- Create: `cmake/FindFilament.cmake`
- Create: `scripts/bootstrap_filament.ps1`
- Create: `scripts/bootstrap_meshoptimizer.ps1`
- Create: `scripts/build_avatar_materials.ps1`
- Create: `tests/scripts/Avatar3dDependencyPinPolicyTest.ps1`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `legal/OSS_BOM.csv`

**Interfaces:**
- Produces: `CS_ENABLE_FILAMENT`, `CS_FILAMENT_ROOT`, `Filament::filament`,
  `Filament::gltfio`, `Filament::filamat`, `Filament::backend`,
  `meshoptimizer::meshoptimizer`, and generated `avatar_materials` resources.
- Consumed by: `cs_avatar_3d_adapter` and release packaging.

- [ ] **Step 1: Add the failing dependency-pin policy test**

```powershell
$filament = Get-Content scripts/bootstrap_filament.ps1 -Raw
$meshopt = Get-Content scripts/bootstrap_meshoptimizer.ps1 -Raw
$materials = Get-Content scripts/build_avatar_materials.ps1 -Raw

if ($filament -notmatch 'v1\.74\.0' -or
    $filament -notmatch '029f2af886174117999d118ea3dfdefafac364d6') {
    throw 'Filament version and commit must be pinned'
}
if ($filament -notmatch 'dafe3e329a44d60c9f9a891c04f94d49b33bd13b2c733a7deefc5f2f39973e0' -or
    $filament -notmatch 'f606f776ad0ac53eb5ed6f266cd7c3f37f18e160bc5cfa7ea291ffd12182f092' -or
    $filament -notmatch '7cb83f5d8a6f60927b6e716e00166ebcad3610e4c9ea7bee4d55ffbf0b1023c1') {
    throw 'Every supported Filament binary archive must have a fixed SHA-256'
}
if ($meshopt -notmatch '9d9890c73011d75920af614485296d1e03e95448' -or
    $meshopt -notmatch 'e40f71b809cdf3361b9a4def85fd44534e8733ce29d4b943c145b76859e4c2b4') {
    throw 'meshoptimizer source and SHA-256 must be pinned'
}
if ($materials -notmatch 'matc' -or $materials -notmatch '--platform') {
    throw 'MToon materials must be compiled by the staged matching matc'
}
```

- [ ] **Step 2: Run and prove the pin test fails**

Run:

```powershell
pwsh -NoProfile -File tests/scripts/Avatar3dDependencyPinPolicyTest.ps1
```

Expected: the scripts do not exist and the policy test fails.

- [ ] **Step 3: Implement audited platform staging**

`bootstrap_filament.ps1` selects exactly one official v1.74.0 release archive:

| Target | Archive SHA-256 |
|---|---|
| Windows desktop | `dafe3e329a44d60c9f9a891c04f94d49b33bd13b2c733a7deefc5f2f39973e0` |
| macOS desktop | `f606f776ad0ac53eb5ed6f266cd7c3f37f18e160bc5cfa7ea291ffd12182f092` |
| Android native | `7cb83f5d8a6f60927b6e716e00166ebcad3610e4c9ea7bee4d55ffbf0b1023c1` |

The script downloads from the official
`https://github.com/google/filament/releases/tag/v1.74.0` release, verifies the
selected archive before extraction, rejects symlinks escaping the staging
directory, and writes `build/filament/prefix/filament-manifest.json` with target,
version, commit, archive URL, archive hash, `matc` hash, and library hashes.

`bootstrap_meshoptimizer.ps1` downloads
`https://github.com/zeux/meshoptimizer/archive/9d9890c73011d75920af614485296d1e03e95448.tar.gz`,
verifies
`e40f71b809cdf3361b9a4def85fd44534e8733ce29d4b943c145b76859e4c2b4`,
builds the static library with tests disabled, and stages its MIT notice.

`FindFilament.cmake` validates the manifest target and exposes imported targets;
it fails configuration when a required library, header, `matc`, or recorded hash
is absent. `build_avatar_materials.ps1` invokes only that staged `matc`, records
the source and output hashes, and emits separate desktop and mobile material
packages.

- [ ] **Step 4: Run the audited bootstrap and configuration tests**

Run:

```powershell
pwsh -NoProfile -File scripts/bootstrap_filament.ps1 -Target windows
pwsh -NoProfile -File scripts/bootstrap_meshoptimizer.ps1
pwsh -NoProfile -File scripts/build_avatar_materials.ps1 -Target desktop
pwsh -NoProfile -File tests/scripts/Avatar3dDependencyPinPolicyTest.ps1
cmake --preset windows-debug -DCS_ENABLE_FILAMENT=ON -DCS_FILAMENT_ROOT="$PWD/build/filament/prefix"
```

Expected: hashes verify, generated material manifests name the staged `matc`,
and CMake reports the exact Filament and meshoptimizer versions.

- [ ] **Step 5: Commit the reproducible 3D toolchain**

```powershell
git add cmake/FindFilament.cmake scripts/bootstrap_filament.ps1 scripts/bootstrap_meshoptimizer.ps1 scripts/build_avatar_materials.ps1 tests/scripts/Avatar3dDependencyPinPolicyTest.ps1 CMakeLists.txt CMakePresets.json legal/OSS_BOM.csv
git commit -m "build(avatar): pin Filament and meshoptimizer toolchains"
```

## Task 2: Parse bounded GLB, VRM 1.0, and Creator Studio rig documents

**Files:**
- Create: `src/avatar_3d_adapter/CMakeLists.txt`
- Create: `src/avatar_3d_adapter/GlbDocument.h`
- Create: `src/avatar_3d_adapter/GlbDocument.cpp`
- Create: `src/avatar_3d_adapter/Vrm1Document.h`
- Create: `src/avatar_3d_adapter/Vrm1Document.cpp`
- Create: `src/avatar_3d_adapter/Vrm1MetaRights.h`
- Create: `src/avatar_3d_adapter/Vrm1MetaRights.cpp`
- Create: `src/avatar_3d_adapter/CreatorStudioRigDocument.h`
- Create: `src/avatar_3d_adapter/CreatorStudioRigDocument.cpp`
- Create: `schemas/creator-studio-rig.schema.json`
- Create: `tests/avatar_3d_adapter/GlbDocumentTest.cpp`
- Create: `tests/avatar_3d_adapter/Vrm1DocumentTest.cpp`
- Create: `tests/avatar_3d_adapter/CreatorStudioRigDocumentTest.cpp`
- Create: `tests/avatar_3d_adapter/Avatar3dMalformedCorpusTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar3d {

struct GlbLimits {
    std::uint64_t maxFileBytes{512ULL * 1024ULL * 1024ULL};
    std::uint32_t maxJsonBytes{32U * 1024U * 1024U};
    std::uint32_t maxNodes{4096};
    std::uint32_t maxMeshes{1024};
    std::uint32_t maxPrimitives{8192};
    std::uint32_t maxJointsPerSkin{512};
    std::uint32_t maxMorphTargetsPerPrimitive{128};
    std::uint32_t maxTextures{512};
    std::uint32_t maxAnimations{256};
    std::uint32_t maxJsonDepth{64};
};

class GlbDocument {
public:
    static core::Result<GlbDocument> parse(std::span<const std::byte> bytes,
                                           const GlbLimits& limits);
    [[nodiscard]] const nlohmann::json& json() const noexcept;
    [[nodiscard]] std::span<const std::byte> binaryChunk() const noexcept;
};

enum class VrmAvatarPermission {
    OnlyAuthor,
    OnlySeparatelyLicensedPerson,
    Everyone
};
enum class VrmCommercialUsage {
    PersonalNonProfit,
    PersonalProfit,
    Corporation
};
enum class VrmCreditNotation { Required, Unnecessary };
enum class VrmModification {
    Prohibited,
    AllowModification,
    AllowModificationRedistribution
};

struct Vrm1MetaRights {
    std::string name;
    std::vector<std::string> authors;
    std::string licenseUrl;
    VrmAvatarPermission avatarPermission;
    VrmCommercialUsage commercialUsage;
    VrmCreditNotation creditNotation;
    bool allowRedistribution;
    VrmModification modification;
};

struct Vrm1Document {
    GlbDocument glb;
    Vrm1MetaRights rights;
    std::unordered_map<std::string, std::uint32_t> humanoidNodeByBone;
    std::unordered_map<std::string, std::uint32_t> expressionIndex;

    static core::Result<Vrm1Document> parse(std::span<const std::byte> bytes,
                                            const GlbLimits& limits);
};

struct CreatorStudioRigDocument {
    std::uint32_t schemaVersion;
    avatar::RigFamily rigFamily;
    std::string masterRigId;
    std::string masterRigVersion;
    std::string skeletonFingerprint;
    std::vector<std::string> bones;
    std::unordered_map<std::string, std::string> semanticBoneMap;
    std::unordered_map<std::string, std::string> expressionMap;

    static core::Result<CreatorStudioRigDocument> parse(
        const nlohmann::json& extensionJson);
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing container and semantic tests**

```cpp
TEST(GlbDocumentTest, RejectsOverflowDuplicateChunksAndOversizedCounts) {
    EXPECT_EQ(parseGlb(glbWithDeclaredLength(UINT32_MAX)).error().code(),
              core::ErrorCode::ParseFailure);
    EXPECT_EQ(parseGlb(glbWithTwoJsonChunks()).error().code(),
              core::ErrorCode::ParseFailure);
    EXPECT_EQ(parseGlb(glbWithNodeCount(4097)).error().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(Vrm1DocumentTest, RequiresVrmCoreHumanoidAndExactMetaEnums) {
    EXPECT_EQ(parseVrm(glbWithoutVrmCore()).error().code(),
              core::ErrorCode::ParseFailure);
    EXPECT_EQ(parseVrm(vrmWithoutRequiredHumanoidBone("hips")).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(parseVrm(vrmWithMetaCommercialUsage("enterprise")).error().code(),
              core::ErrorCode::ParseFailure);
}

TEST(CreatorStudioRigDocumentTest, RejectsHumanoidDisguisedAsNonHumanoid) {
    const auto result = parseCreatorRig(
        creatorRigJson(RigFamily::Quadruped).withSemanticBone("hips", "Hips"));
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove the parser tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R "GlbDocumentTest|Vrm1DocumentTest|CreatorStudioRigDocumentTest" --output-on-failure
```

Expected: the target or new document types are absent.

- [ ] **Step 3: Implement strict format separation**

`GlbDocument::parse` validates the glTF magic, version 2, four-byte alignment,
declared file length, one JSON chunk, at most one BIN chunk, UTF-8 JSON, JSON
depth, all index references, accessor byte ranges, buffer-view ranges, image
payload ranges, and every `GlbLimits` count before exposing spans.

`Vrm1Document::parse` requires `VRMC_vrm` in both `extensionsUsed` and the root
extensions object, validates `specVersion` against supported VRM 1.0 values,
checks the full required humanoid bone set and uniqueness, validates expression
bind indices, and parses the exact required `meta` fields:
`name`, `authors`, `licenseUrl`, `avatarPermission`, `commercialUsage`,
`creditNotation`, `allowRedistribution`, and `modification`. Unknown enum
strings fail closed.

`CreatorStudioRigDocument::parse` requires root extension
`CS_avatar_rig`, schema version 1, a non-humanoid-compatible `RigFamily`, an
exact semantic bone map, and a fingerprint over normalized bone names, parents,
bind transforms, and units. Its JSON Schema sets `additionalProperties: false`.
The animal-rig plan later supplies family-specific required semantic bones.

- [ ] **Step 4: Run parser tests and malformed-input corpus**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R "GlbDocumentTest|Vrm1DocumentTest|CreatorStudioRigDocumentTest" --output-on-failure
ctest --test-dir build/windows-debug -R Avatar3dMalformedCorpusTest --output-on-failure
```

Expected: valid minimal VRM and Creator Studio glTF fixtures pass; truncation,
overflow, bad indices, unsupported versions, and mixed rig semantics fail with
stable error codes and no out-of-bounds sanitizer report.

- [ ] **Step 5: Commit the truthful 3D document boundary**

```powershell
git add src/avatar_3d_adapter/CMakeLists.txt src/avatar_3d_adapter/GlbDocument.h src/avatar_3d_adapter/GlbDocument.cpp src/avatar_3d_adapter/Vrm1Document.h src/avatar_3d_adapter/Vrm1Document.cpp src/avatar_3d_adapter/Vrm1MetaRights.h src/avatar_3d_adapter/Vrm1MetaRights.cpp src/avatar_3d_adapter/CreatorStudioRigDocument.h src/avatar_3d_adapter/CreatorStudioRigDocument.cpp schemas/creator-studio-rig.schema.json tests/avatar_3d_adapter/GlbDocumentTest.cpp tests/avatar_3d_adapter/Vrm1DocumentTest.cpp tests/avatar_3d_adapter/CreatorStudioRigDocumentTest.cpp tests/avatar_3d_adapter/Avatar3dMalformedCorpusTest.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): validate VRM and nonhumanoid glTF separately"
```

## Task 3: Define compatible modular 3D assets

**Files:**
- Create: `src/avatar_3d_adapter/Avatar3dModuleManifest.h`
- Create: `src/avatar_3d_adapter/Avatar3dModuleManifest.cpp`
- Create: `src/avatar_3d_adapter/Avatar3dModuleManifestCodec.h`
- Create: `src/avatar_3d_adapter/Avatar3dModuleManifestCodec.cpp`
- Create: `schemas/avatar-3d-module.schema.json`
- Create: `tests/avatar_3d_adapter/Avatar3dModuleManifestTest.cpp`
- Modify: `src/avatar_3d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar3d {

enum class MaterialSemantic {
    Skin,
    EyeWhite,
    Iris,
    Hair,
    Fabric,
    Metal,
    Accessory,
    Transparent
};

struct BoneBinding {
    std::string moduleBone;
    std::string masterBone;
    std::array<float, 16> inverseBindMatrix;
};

struct MorphBinding {
    std::string moduleMorph;
    std::string semanticExpression;
    float scale;
};

struct Avatar3dModuleManifest {
    std::uint32_t schemaVersion;
    avatar::AvatarAssetId assetId;
    avatar::AvatarSlot slot;
    avatar::RigFamily rigFamily;
    std::string masterRigId;
    std::string masterRigVersion;
    std::string skeletonFingerprint;
    float metersPerUnit;
    std::string coordinateSystem;
    std::filesystem::path glbPath;
    std::vector<BoneBinding> bones;
    std::vector<MorphBinding> morphs;
    std::vector<std::string> hiddenBaseMeshPrimitives;
    std::unordered_map<std::string, MaterialSemantic> materials;
    std::vector<float> authoredLodRatios;
};

class Avatar3dModuleManifestCodec {
public:
    static core::Result<Avatar3dModuleManifest> decode(
        const nlohmann::json& json,
        const avatar::ResolvedAvatarAsset& asset);
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing manifest compatibility tests**

```cpp
TEST(Avatar3dModuleManifestTest, AcceptsExactCompatibleHairModule) {
    const auto result = decodeModule(
        validModuleJson(AvatarSlot::HairFront, RigFamily::Humanoid));
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().metersPerUnit, 1.0F);
}

TEST(Avatar3dModuleManifestTest, RejectsRigBindPoseAndPathEscapes) {
    EXPECT_EQ(decodeModule(moduleWithRigFamily(RigFamily::Quadruped)).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(decodeModule(moduleWithChangedBindMatrix()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(decodeModule(moduleWithGlbPath("../../outside.glb")).error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove manifest tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dModuleManifestTest --output-on-failure
```

Expected: the manifest and codec do not exist.

- [ ] **Step 3: Implement schema-backed compatibility validation**

The codec validates `avatar-3d-module.schema.json`, resolves `glbPath` through
the signed package file table, parses the GLB, and verifies:

1. the selected slot accepts the module;
2. rig family, master-rig ID/version, skeleton fingerprint, coordinate system,
   and one-meter units match;
3. each skinned vertex has no more than eight non-zero weights and normalized
   weight sum within `0.001`;
4. each referenced master bone exists and each inverse bind matrix matches the
   authored base within `1e-5`;
5. morphs and material semantics are declared and unique;
6. hidden primitive names exist in the base mesh;
7. authored LOD ratios are strictly descending in `(0, 1]`;
8. no module can load an undeclared texture, external URI, animation, or script.

The schema uses explicit enums and `additionalProperties: false`. Decoder error
messages include package ID, asset ID, slot, and failing field without exposing
arbitrary package paths to UI text.

- [ ] **Step 4: Run compatibility and schema tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dModuleManifestTest --output-on-failure
```

Expected: valid modules decode; cross-family clothing, mismatched bind poses,
unknown morphs, external URIs, undeclared textures, and path escapes fail.

- [ ] **Step 5: Commit the modular 3D contract**

```powershell
git add src/avatar_3d_adapter/Avatar3dModuleManifest.h src/avatar_3d_adapter/Avatar3dModuleManifest.cpp src/avatar_3d_adapter/Avatar3dModuleManifestCodec.h src/avatar_3d_adapter/Avatar3dModuleManifestCodec.cpp schemas/avatar-3d-module.schema.json tests/avatar_3d_adapter/Avatar3dModuleManifestTest.cpp src/avatar_3d_adapter/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): define compatible modular 3d assets"
```

## Task 4: Compose meshes, skeletons, morphs, materials, and LODs

**Files:**
- Create: `src/avatar_3d_adapter/Avatar3dModuleComposer.h`
- Create: `src/avatar_3d_adapter/Avatar3dModuleComposer.cpp`
- Create: `tests/avatar_3d_adapter/Avatar3dModuleComposerTest.cpp`
- Modify: `src/avatar_3d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar3d {

struct Compiled3dPrimitive {
    std::string stableName;
    std::vector<std::byte> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t materialIndex;
    std::vector<std::string> morphNames;
    std::vector<std::vector<std::byte>> lodVertexBuffers;
    std::vector<std::vector<std::uint32_t>> lodIndexBuffers;
};

struct Compiled3dScene {
    avatar::AvatarRepresentation representation;
    avatar::RigFamily rigFamily;
    std::string skeletonFingerprint;
    std::vector<std::string> jointNames;
    std::vector<std::array<float, 16>> inverseBindMatrices;
    std::vector<Compiled3dPrimitive> primitives;
    std::unordered_map<std::string, std::uint32_t> materialBySemantic;
    std::unordered_map<std::string, std::vector<MorphBinding>> expressions;
    std::string deterministicDigest;
};

class Avatar3dModuleComposer {
public:
    core::Result<Compiled3dScene> compose(
        const avatar::AvatarSpec& spec,
        const avatar::ResolvedAvatarAsset& base,
        std::span<const avatar::ResolvedAvatarAsset> modules,
        avatar::AvatarQualityTier quality) const;
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing deterministic composition tests**

```cpp
TEST(Avatar3dModuleComposerTest, RebindsModulesAndHidesCoveredBasePrimitives) {
    const auto scene = composer().compose(
        humanoidSpec(), baseBody(), {jacket(), layeredHair()},
        AvatarQualityTier::DesktopHigh);
    ASSERT_TRUE(scene.hasValue()) << scene.error().message();
    EXPECT_FALSE(hasPrimitive(scene.value(), "base_torso"));
    EXPECT_TRUE(hasPrimitive(scene.value(), "jacket_outer"));
    EXPECT_TRUE(allJointIndicesAreInRange(scene.value()));
    EXPECT_TRUE(allWeightsAreNormalized(scene.value()));
}

TEST(Avatar3dModuleComposerTest, ProducesDeterministicOptimizedLods) {
    const auto first = composeFixture();
    const auto second = composeFixtureWithReversedCatalogIteration();
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().deterministicDigest,
              second.value().deterministicDigest);
    EXPECT_TRUE(lodTriangleCountsStrictlyDecrease(first.value()));
}
```

- [ ] **Step 2: Run and prove composition tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dModuleComposerTest --output-on-failure
```

Expected: the composer and scene types are absent.

- [ ] **Step 3: Implement deterministic scene composition**

The composer:

1. sorts selected assets by `AvatarSlot` then stable asset ID;
2. remaps module joint indices to the master skeleton and verifies bind matrices;
3. removes only explicitly named covered base primitives;
4. preserves vertex tangents, UVs, colors, eight-weight skinning, and morph deltas;
5. creates semantic material instances and applies `AvatarSpec::materials` and
   color overrides without mutating source packages;
6. uses `meshopt_generateVertexRemap`, vertex-cache optimization, overdraw
   optimization, and vertex-fetch optimization in a fixed order;
7. generates 1.0, 0.65, 0.35, and 0.18 LODs when equivalent authored LODs are
   absent, preserving UV seams, bone boundaries, morph deltas, and silhouette
   error thresholds;
8. serializes canonical metadata and buffer order before computing SHA-256.

Composition does not merge unrelated skeletons, guess bone mappings from names,
or discard a requested module to make an incompatible selection appear valid.

- [ ] **Step 4: Run composition and determinism tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dModuleComposerTest --output-on-failure
```

Expected: meshes deform against the master skeleton, coverage rules are exact,
LOD counts decline without invalid indices, and repeated output hashes match.

- [ ] **Step 5: Commit the real 3D composer**

```powershell
git add src/avatar_3d_adapter/Avatar3dModuleComposer.h src/avatar_3d_adapter/Avatar3dModuleComposer.cpp tests/avatar_3d_adapter/Avatar3dModuleComposerTest.cpp src/avatar_3d_adapter/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): compose skinned 3d modules and lods"
```

## Task 5: Render the compiled scene with Filament and MToon materials

**Files:**
- Create: `materials/avatar/mtoon_opaque.mat`
- Create: `materials/avatar/mtoon_transparent.mat`
- Create: `materials/avatar/mtoon_outline.mat`
- Create: `src/avatar_3d_adapter/FilamentMaterialLibrary.h`
- Create: `src/avatar_3d_adapter/FilamentMaterialLibrary.cpp`
- Create: `src/avatar_3d_adapter/FilamentAvatarRenderer.h`
- Create: `src/avatar_3d_adapter/FilamentAvatarRenderer.cpp`
- Create: `src/avatar_3d_adapter/FilamentAvatarRendererFactory.h`
- Create: `src/avatar_3d_adapter/FilamentAvatarRendererFactory.cpp`
- Create: `tests/avatar_3d_adapter/FilamentAvatarRendererTest.cpp`
- Create: `tests/avatar_3d_adapter/FilamentAvatarRendererLifetimeTest.cpp`
- Modify: `src/avatar/AvatarRenderFrame.h`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `src/avatar_3d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class AvatarFrameStorage { CpuBgra, GpuTexture };

struct AvatarGpuTexture {
    void* nativeHandle;
    std::uint64_t generation;
    std::uint32_t width;
    std::uint32_t height;
    media::PixelFormat pixelFormat;
};

} // namespace creator::avatar

namespace creator::avatar3d {

class FilamentAvatarRenderer final : public avatar::IAvatarRenderer {
public:
    core::Result<avatar::AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const avatar::AvatarParameterValue> parameters) override;
    core::Result<void> resize(std::uint32_t width, std::uint32_t height);
    core::Result<void> setQuality(avatar::AvatarQualityTier quality);
};

class FilamentAvatarRendererFactory {
public:
    core::Result<std::unique_ptr<avatar::IAvatarRenderer>> create(
        const avatar::CompiledAvatarArtifact& artifact,
        const avatar::AvatarRendererOptions& options) const;
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing real-render tests**

```cpp
TEST(FilamentAvatarRendererTest, SkinMorphCameraAndLightChangeRealPixels) {
    auto renderer = openFixtureRenderer(512, 512);
    const auto neutral = renderReadback(renderer, neutralPose());
    const auto posed = renderReadback(renderer, smileBlinkTurnAndWave());
    ASSERT_TRUE(neutral.hasValue());
    ASSERT_TRUE(posed.hasValue());
    EXPECT_NE(core::sha256(neutral.value().pixels),
              core::sha256(posed.value().pixels));
    EXPECT_TRUE(hasCelBands(posed.value()));
    EXPECT_TRUE(hasOutlinePixels(posed.value()));
}

TEST(FilamentAvatarRendererTest, PreservesStraightAlphaTransparency) {
    const auto frame = renderReadback(openFixtureRenderer(320, 240), neutralPose());
    ASSERT_TRUE(frame.hasValue());
    EXPECT_EQ(frame.value().pixelFormat, media::PixelFormat::Bgra8);
    EXPECT_TRUE(hasZeroAlphaBackground(frame.value()));
    EXPECT_TRUE(allRgbChannelsZeroWhereAlphaIsZero(frame.value()));
}
```

- [ ] **Step 2: Run and prove the renderer tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R FilamentAvatarRendererTest --output-on-failure
```

Expected: Filament renderer and materials are absent.

- [ ] **Step 3: Implement the production rendering path**

`FilamentMaterialLibrary` verifies the generated material manifest and loads
opaque, transparent, and outline packages compiled by the staged v1.74.0
`matc`. The MToon-compatible inputs include base color/texture, shade color and
threshold, normal map, emissive, rim color/lift/fresnel, UV animation, culling,
alpha mode/cutoff, outline width/color, and per-material render queue.

`FilamentAvatarRenderer` creates one engine-owned scene, skeleton entity,
renderable entity per compiled primitive, morph target buffers, orthographic or
perspective avatar camera, key/fill/rim lights, transparent render target, and
quality-tier resources. Each frame:

1. maps canonical avatar parameters to joint local transforms and morph weights;
2. updates skinning and morph buffers before render;
3. advances only bounded physics deltas derived from the timestamp;
4. chooses the requested LOD without rebuilding the scene;
5. renders straight-alpha RGBA/BGRA with a transparent clear color;
6. returns a generation-counted GPU texture and schedules CPU readback only
   when a consumer explicitly requests it.

The renderer uses no static preview image, synthetic test pattern, separate
QML-only mesh, or CPU color rectangle as a success path. Device loss returns
`ErrorCode::IoFailure` with issue code `avatar.render.device_lost`; the caller
retains the last good compiled artifact and
may recreate the renderer once.

- [ ] **Step 4: Run render, leak, and backend tests**

Run:

```powershell
pwsh -NoProfile -File scripts/build_avatar_materials.ps1 -Target desktop
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R FilamentAvatarRendererTest --output-on-failure
ctest --test-dir build/windows-debug -R FilamentAvatarRendererLifetimeTest --output-on-failure
```

Expected: the committed real mesh produces stable golden images within the
documented tolerance on supported backends; pose and expression pixels differ,
alpha is valid, and 100 create/render/destroy cycles leak no Filament resources.

- [ ] **Step 5: Commit the Filament renderer**

```powershell
git add materials/avatar/mtoon_opaque.mat materials/avatar/mtoon_transparent.mat materials/avatar/mtoon_outline.mat src/avatar_3d_adapter/FilamentMaterialLibrary.h src/avatar_3d_adapter/FilamentMaterialLibrary.cpp src/avatar_3d_adapter/FilamentAvatarRenderer.h src/avatar_3d_adapter/FilamentAvatarRenderer.cpp src/avatar_3d_adapter/FilamentAvatarRendererFactory.h src/avatar_3d_adapter/FilamentAvatarRendererFactory.cpp tests/avatar_3d_adapter/FilamentAvatarRendererTest.cpp tests/avatar_3d_adapter/FilamentAvatarRendererLifetimeTest.cpp src/avatar/AvatarRenderFrame.h src/avatar/CMakeLists.txt src/avatar_3d_adapter/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): render cel shaded avatars with Filament"
```

## Task 6: Compile and cache complete 3D avatars through the common contract

**Files:**
- Create: `src/avatar_3d_adapter/Avatar3dAvatarCompiler.h`
- Create: `src/avatar_3d_adapter/Avatar3dAvatarCompiler.cpp`
- Create: `src/avatar_3d_adapter/Avatar3dCompileCache.h`
- Create: `src/avatar_3d_adapter/Avatar3dCompileCache.cpp`
- Create: `tests/avatar_3d_adapter/Avatar3dAvatarCompilerTest.cpp`
- Create: `tests/avatar_3d_adapter/Avatar3dCompileCacheTest.cpp`
- Modify: `src/avatar/AvatarModelDescriptor.h`
- Modify: `src/avatar/AvatarModelDescriptor.cpp`
- Modify: `src/avatar_3d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar3d {

class Avatar3dAvatarCompiler final : public avatar::IAvatarCompiler {
public:
    core::Result<avatar::CompiledAvatarArtifact> compile(
        const avatar::AvatarCompileRequest& request) override;
};

class Avatar3dCompileCache {
public:
    core::Result<std::optional<avatar::CompiledAvatarArtifact>> lookup(
        std::string_view cacheKey) const;
    core::Result<void> publish(
        std::string_view cacheKey,
        const avatar::CompiledAvatarArtifact& artifact);
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing compiler and cache tests**

```cpp
TEST(Avatar3dAvatarCompilerTest, CompilesHumanoidAndQuadrupedToDifferentRepresentations) {
    const auto human = compiler().compile(requestFor(humanoidSpec()));
    const auto animal = compiler().compile(requestFor(quadrupedSpec()));
    ASSERT_TRUE(human.hasValue());
    ASSERT_TRUE(animal.hasValue());
    EXPECT_EQ(human.value().representation,
              AvatarRepresentation::Vrm1);
    EXPECT_EQ(animal.value().representation,
              AvatarRepresentation::GltfRig);
}

TEST(Avatar3dAvatarCompilerTest, RejectsStaleOrTamperedCacheEntry) {
    seedCacheWithChangedArtifactByte(validRequest());
    const auto result = compiler().compile(validRequest());
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().wasRebuilt);
    EXPECT_TRUE(cacheEntryMatchesManifest(result.value()));
}
```

- [ ] **Step 2: Run and prove compiler tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dAvatarCompilerTest --output-on-failure
```

Expected: the compiler and cache types are absent.

- [ ] **Step 3: Implement compile orchestration and atomic cache publication**

The compiler validates `AvatarSpec`, resolves every asset through
`IAvatarCatalog`, verifies package signatures and hashes, decodes manifests,
composes the scene, serializes a renderer-owned artifact, and writes an
`AvatarModelDescriptor` containing representation, rig family, skeleton
fingerprint, expression semantics, quality tiers, source asset versions/hashes,
compiler version, Filament ABI, meshoptimizer version, and artifact SHA-256.

The cache key is SHA-256 over canonical `AvatarSpec`, sorted asset hashes,
compiler version, material package hashes, Filament ABI, meshoptimizer version,
quality tier, target OS, target architecture, and graphics backend. Cache
publication writes to a sibling temporary directory, fsyncs files, verifies the
manifest, and atomically renames it. Cancellation removes only the uncommitted
temporary directory. A bad cache entry is quarantined and rebuilt; the last good
renderer stays alive until a replacement opens successfully.

- [ ] **Step 4: Run compiler, cancellation, and cache tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R "Avatar3dAvatarCompilerTest|Avatar3dCompileCacheTest" --output-on-failure
```

Expected: humanoid and animal artifacts retain truthful representations,
repeated compiles hit byte-identical cache entries, cancellation is atomic, and
tampered entries rebuild.

- [ ] **Step 5: Commit common-contract 3D compilation**

```powershell
git add src/avatar_3d_adapter/Avatar3dAvatarCompiler.h src/avatar_3d_adapter/Avatar3dAvatarCompiler.cpp src/avatar_3d_adapter/Avatar3dCompileCache.h src/avatar_3d_adapter/Avatar3dCompileCache.cpp tests/avatar_3d_adapter/Avatar3dAvatarCompilerTest.cpp tests/avatar_3d_adapter/Avatar3dCompileCacheTest.cpp src/avatar/AvatarModelDescriptor.h src/avatar/AvatarModelDescriptor.cpp src/avatar_3d_adapter/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): compile and cache modular 3d avatars"
```

## Task 7: Export only valid formats under current rights

**Files:**
- Create: `src/avatar_3d_adapter/Avatar3dExporter.h`
- Create: `src/avatar_3d_adapter/Avatar3dExporter.cpp`
- Create: `scripts/validate_avatar_exports.ps1`
- Create: `tests/avatar_3d_adapter/Avatar3dExporterTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `src/avatar_3d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar3d {

enum class Avatar3dExportFormat { Vrm1, Gltf2WithCreatorRig };

struct Avatar3dExportRequest {
    avatar::AvatarSpec spec;
    Avatar3dExportFormat format;
    std::filesystem::path destination;
    std::string displayName;
    std::vector<std::string> authors;
};

struct Avatar3dExportReceipt {
    std::vector<std::filesystem::path> files;
    std::string outputSha256;
    avatar::AvatarRightsDecision rightsDecision;
    std::string rightsSnapshotSha256;
};

class Avatar3dExporter {
public:
    core::Result<Avatar3dExportReceipt> exportAvatar(
        const Avatar3dExportRequest& request,
        const avatar::CompiledAvatarArtifact& artifact);
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing format and rights tests**

```cpp
TEST(Avatar3dExporterTest, ExportsHumanoidAsValidVrmWithCurrentRights) {
    const auto receipt = exporter().exportAvatar(
        vrmRequest(humanoidSpec()), compiledHumanoid());
    ASSERT_TRUE(receipt.hasValue()) << receipt.error().message();
    const auto parsed = Vrm1Document::parse(readBytes(receipt.value().files.at(0)),
                                            GlbLimits{});
    ASSERT_TRUE(parsed.hasValue()) << parsed.error().message();
    EXPECT_EQ(parsed.value().rights.commercialUsage,
              VrmCommercialUsage::Corporation);
}

TEST(Avatar3dExporterTest, NeverLabelsQuadrupedAsVrm) {
    const auto result = exporter().exportAvatar(
        vrmRequest(quadrupedSpec()), compiledQuadruped());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(Avatar3dExporterTest, RechecksChangedAssetRightsAtExportTime) {
    installRightsRevisionThatBlocksModelExport();
    const auto result = exporter().exportAvatar(
        vrmRequest(humanoidSpec()), previouslyCompiledHumanoid());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidState);
    EXPECT_FALSE(destinationExists());
}
```

- [ ] **Step 2: Run and prove exporter tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dExporterTest --output-on-failure
```

Expected: the exporter type is absent.

- [ ] **Step 3: Implement transactional truthful export**

At invocation time, the exporter resolves the current installed asset revisions
and calls
`AvatarLicenseResolver::resolve` with an `AvatarUseContext` whose `useKind` is
`UseKind::ModelExport`. It fails before
opening a destination file when any asset blocks export, requires missing
attribution, has an expired rights record, or differs from the compiled source
hash.

For `Vrm1`, the exporter requires `RigFamily::Humanoid`,
`RigFamily::Kemonomimi`, or a VRM-compatible `RigFamily::AnthroBiped`, validates
the complete humanoid map, writes `VRMC_vrm` 1.0 metadata using the strictest
transitive permissions, preserves MToon expressions and spring data, embeds a
Creator Studio rights snapshot hash in `extras`, then parses its own output with
`Vrm1Document`.

For `Gltf2WithCreatorRig`, the exporter writes `<name>.glb` with
`CS_avatar_rig` and `<name>.creator-studio-rig.json` with the schema version,
rig family, semantic bones, expressions, source hashes, license attribution,
and rights snapshot hash. It does not write `VRMC_vrm`.

Both formats write into a destination sibling temporary directory, verify all
output hashes and schemas, then atomically publish. Failure leaves no partial
files and never overwrites an existing export without the caller's explicit
replace flag.

- [ ] **Step 4: Run exporter and external conformance checks**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dExporterTest --output-on-failure
pwsh -NoProfile -File scripts/validate_avatar_exports.ps1 -Input tests/output/avatar-3d
```

Expected: allowed humanoids round-trip as VRM 1.0, allowed animals round-trip as
glTF plus Creator Studio metadata, forbidden combinations create no output, and
all files match their receipts.

- [ ] **Step 5: Commit rights-aware 3D export**

```powershell
git add src/avatar_3d_adapter/Avatar3dExporter.h src/avatar_3d_adapter/Avatar3dExporter.cpp tests/avatar_3d_adapter/Avatar3dExporterTest.cpp src/avatar/CMakeLists.txt src/avatar_3d_adapter/CMakeLists.txt tests/CMakeLists.txt scripts/validate_avatar_exports.ps1
git commit -m "feat(avatar): export truthful rights checked 3d formats"
```

## Task 8: Prove production 3D creation with licensed humanoid and animal rigs

**Files:**
- Create: `tests/fixtures/avatar-3d/README.md`
- Create: `tests/acceptance/Production3dAvatarAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`
- Modify: `legal/ASSET_BOM.csv`

**Interfaces:**
- Consumes: foundation catalog, 3D compiler, Filament renderer, exporter.
- Produces: final-quality humanoid and non-humanoid 3D release gate.

- [ ] **Step 1: Add the failing end-to-end production tests**

```cpp
TEST(Production3dAvatarAcceptanceTest, CustomizesAndRendersRealCelShadedHumanoid) {
    const auto spec = fixtureHumanoidSpec()
        .withHair("core3d.hair.layered-bob")
        .withEyeColor("#7B61FF")
        .withOuterwear("core3d.outfit.modern-jacket")
        .withAccessory("core3d.accessory.star-earrings")
        .build();
    const auto compiled = compiler().compile(requestFor(spec));
    ASSERT_TRUE(compiled.hasValue()) << compiled.error().message();
    auto renderer = openFilamentRenderer(compiled.value());
    ASSERT_TRUE(renderer.hasValue()) << renderer.error().message();
    const auto neutral = readback(renderer.value()->render(t0(), neutralPose()));
    const auto motion = readback(renderer.value()->render(t1(), smileBlinkTurnAndWave()));
    ASSERT_TRUE(neutral.hasValue());
    ASSERT_TRUE(motion.hasValue());
    EXPECT_NE(core::sha256(neutral.value().pixels),
              core::sha256(motion.value().pixels));
    EXPECT_TRUE(hasCelBandsAndOutline(motion.value()));
}

TEST(Production3dAvatarAcceptanceTest, RendersAnimalAndExportsItOnlyAsGltfRig) {
    const auto compiled = compiler().compile(requestFor(fixtureAnimalSpec()));
    ASSERT_TRUE(compiled.hasValue()) << compiled.error().message();
    EXPECT_EQ(compiled.value().representation,
              AvatarRepresentation::GltfRig);
    EXPECT_TRUE(renderedPoseDiffers(compiled.value(), quadrupedMotion()));
    EXPECT_EQ(tryVrmExport(compiled.value()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(gltfRigExportRoundTrips(compiled.value()));
}
```

- [ ] **Step 2: Run and prove the production gate fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_production_3d_avatar_acceptance_tests
ctest --test-dir build/windows-debug -R Production3dAvatarAcceptanceTest --output-on-failure
```

Expected: licensed final-quality humanoid and non-humanoid fixture packages are
absent.

- [ ] **Step 3: Add two signed, final-quality acceptance rigs and rights records**

The humanoid fixture contains a complete VRM 1.0 humanoid skeleton, face
expressions, visemes, eye aim, eight-weight skinning, layered hair, swappable
jacket, transparent accessories, MToon materials, spring bones, and four LODs.
The non-humanoid fixture contains a true quadruped skeleton, paw and spine
motion, ears, tail, muzzle expressions, cel-shaded materials, and four LODs; it
contains `CS_avatar_rig` and no `VRMC_vrm`.

Neither fixture is a cube, triangle, test chart, static rendered image, or
renamed public sample. Each signed package has a `legal/ASSET_BOM.csv` record
with creator, contract/license evidence, commercial broadcast, derivative
character, application bundle, test redistribution, model export, attribution,
asset hashes, and review date. The rights resolver must permit the acceptance
use case before the tests load either model.

- [ ] **Step 4: Run complete 3D and regression gates**

Run:

```powershell
cmake --preset windows-debug -DCS_ENABLE_AVATAR_PACKS=ON -DCS_SODIUM_ROOT="$PWD/build/sodium/prefix" -DCS_ENABLE_FILAMENT=ON -DCS_FILAMENT_ROOT="$PWD/build/filament/prefix"
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug -R "Production3dAvatarAcceptanceTest|Avatar3d.*Test|FilamentAvatarRendererTest" --output-on-failure
ctest --test-dir build/windows-debug --output-on-failure
git diff --check
```

Expected: both real rigs compile and render, deterministic motion changes real
pixels, alpha and cel shading pass golden checks, truthful exports round-trip,
and the complete suite passes.

- [ ] **Step 5: Commit the production 3D gate**

```powershell
git add tests/fixtures/avatar-3d/README.md tests/acceptance/Production3dAvatarAcceptanceTest.cpp tests/CMakeLists.txt README.md legal/ASSET_BOM.csv
git commit -m "test(avatar): qualify real humanoid and animal 3d creation"
```

## Plan Completion Gate

- Verified Filament and meshoptimizer binaries/sources are reproducibly staged
  with matching licenses and material compiler.
- Bounded parsers distinguish VRM 1.0 humanoids from Creator Studio
  non-humanoid glTF rigs and reject ambiguous files.
- Signed compatible modules compose into skinned meshes, expressions,
  cel-shaded materials, and deterministic LODs.
- The actual Filament renderer drives skinning, morphs, materials, outlines,
  transparent output, and GPU frames.
- Humanoid VRM and non-humanoid glTF export re-evaluate current transitive
  rights and publish atomically.
- Final-quality licensed humanoid and animal rigs pass end-to-end rendering and
  truthful format acceptance; no synthetic or static substitute can satisfy the
  gate.
