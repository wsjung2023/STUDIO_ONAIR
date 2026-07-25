# Production 2D Avatar Creator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile signed, pre-rigged modular art into real Inochi2D puppets, apply detailed colors, expressions, and physics, and render the result through the existing avatar pipeline.

**Architecture:** Treat every 2D part as an authored rig module attached to a declared master-rig slot, not as a flat PNG overlay. A Qt-free compiler reads bounded INP containers, namespaces node IDs, merges modules, bakes color-mask textures, writes a deterministic compiled INP, and caches it by specification and asset hashes. The existing Inochi2D C-FFI runtime remains the source of deformation and draw lists; software rendering proves correctness while the later workspace/output plan supplies the zero-copy production GPU path.

**Tech Stack:** C++20, Inochi2D v0.8.7 (`e2235f6f29688d4013a0eec116a37ec8af8f1fb5`, archive SHA-256 `cf0b7bdedc61452614f44f497f0165c3da76ce0b5c7c8b59a29c6fa45e980aa2`), nlohmann/json 3.11.3, stb commit `31c1ad37456438565541f4919958214b6e762fb4` (archive SHA-256 `e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319`), GoogleTest 1.15.2.

## Global Constraints

- Requires completion of `2026-07-24-avatar-platform-foundation.md`.
- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- `cs_avatar` and the new `cs_avatar_2d_adapter` stay Qt-free.
- Inochi2D SDK and every bundled model are separate license records.
- Full drawing and arbitrary rig authoring are out of scope; every selectable module is already rigged for a declared slot and master-rig version.
- A module cannot reference paths, node IDs, textures, parameters, or masks outside its signed package declaration.
- The compiler accepts INP big-endian containers with `TRNSRTS\0`, `TEX_SECT`, and optional `EXT_SECT`; it rejects malformed lengths before allocation.
- Compiler output is deterministic for the same `AvatarSpec`, asset hashes, compiler version, and target quality.
- The UI never previews a different renderer; later QML consumes this plan's actual compiled model and renderer.
- Existing `AvatarSoftwareRasterizer` remains a correctness fallback, not the final 1080p60 performance claim.

---

## File Structure

```text
src/avatar/
  AvatarCompileTypes.h
  IAvatarCompiler.h
  AvatarModelDescriptor.h/.cpp              # extend compiled metadata
src/avatar_2d_adapter/
  CMakeLists.txt
  InpContainer.h/.cpp
  Inochi2dModuleManifest.h/.cpp
  Inochi2dModuleManifestCodec.h/.cpp
  Inochi2dTextureBaker.h/.cpp
  Inochi2dModuleComposer.h/.cpp
  Inochi2dAvatarCompiler.h/.cpp
  Inochi2dCompileCache.h/.cpp
src/avatar/inochi2d/
  Inochi2dRuntimeManifest.h/.cpp
  Inochi2dModelRuntime.h/.cpp
  Inochi2dAvatarRenderer.h/.cpp
schemas/
  inochi2d-module.schema.json
scripts/
  bootstrap_inochi2d.ps1
  verify_inochi2d_runtime.ps1
tests/avatar_2d_adapter/
  InpContainerTest.cpp
  Inochi2dModuleManifestTest.cpp
  Inochi2dTextureBakerTest.cpp
  Inochi2dModuleComposerTest.cpp
  Inochi2dAvatarCompilerTest.cpp
tests/acceptance/
  Production2dAvatarAcceptanceTest.cpp
```

## Task 1: Pin and verify the Inochi2D C-FFI runtime

**Files:**
- Create: `scripts/bootstrap_inochi2d.ps1`
- Create: `scripts/verify_inochi2d_runtime.ps1`
- Create: `src/avatar/inochi2d/Inochi2dRuntimeManifest.h`
- Create: `src/avatar/inochi2d/Inochi2dRuntimeManifest.cpp`
- Create: `tests/avatar/inochi2d/Inochi2dRuntimeManifestTest.cpp`
- Create: `tests/scripts/Inochi2dBootstrapPolicyTest.ps1`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `legal/OSS_BOM.csv`

**Interfaces:**
- Produces: option `CS_ENABLE_INOCHI2D`, `CS_INOCHI2D_ROOT`,
  `Inochi2dRuntimeManifest::loadAndVerify(root)`.
- Consumed by: renderer factory and release packaging.
- The audited bootstrap target is currently `windows-x64` only. Mach-O and ELF
  remain explicitly unsupported until bounded binary parsers and physical
  target builds exist.

- [ ] **Step 1: Add the failing pin and runtime-manifest tests**

```cpp
TEST(Inochi2dRuntimeManifestTest, RejectsChangedLibraryAndMissingSymbolList) {
    const auto root = writeValidRuntimeFixture();
    overwriteByte(root / runtimeLibraryName());
    EXPECT_EQ(Inochi2dRuntimeManifest::loadAndVerify(root).error().code(),
              core::ErrorCode::IoFailure);

    writeValidRuntimeFixtureWithoutSymbol("in_puppet_draw");
    EXPECT_EQ(Inochi2dRuntimeManifest::loadAndVerify(root).error().code(),
              core::ErrorCode::UnsupportedVersion);
}
```

The PowerShell policy test requires the exact source URL and SHA-256 from this
plan.

- [ ] **Step 2: Run and prove the tests fail**

Run:

```powershell
pwsh -NoProfile -File tests/scripts/Inochi2dBootstrapPolicyTest.ps1
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R Inochi2dRuntimeManifestTest --output-on-failure
```

Expected: pin policy fails and the C++ type does not exist.

- [ ] **Step 3: Implement audited runtime staging**

`bootstrap_inochi2d.ps1` must:

1. download `https://github.com/Inochi2D/inochi2d/archive/refs/tags/v0.8.7.tar.gz`;
2. verify `cf0b7bdedc61452614f44f497f0165c3da76ce0b5c7c8b59a29c6fa45e980aa2`;
3. build the dynamic C-FFI with LDC release mode for the audited
   MSVC-compatible Windows x64 target;
4. stage only the C-FFI runtime library and BSD-2-Clause notice under
   `build/inochi2d/prefix/<target>`;
5. write `runtime-manifest.json` with version, commit, archive hash, library
   hash, target triple, minimum OS/API, compiler/SDK identity, ABI mode
   `IN_VEC2_POSITION`, and the 16 exact symbols currently resolved by
   `Inochi2dModelRuntime.cpp`.

The verifier rejects extra DLLs, wrong ABI mode, missing actual PE exports,
unapproved imports, changed hashes, or a target/architecture mismatch before
`LoadLibraryExW`. Mach-O and ELF remain fail-closed and are not release claims.

- [ ] **Step 4: Run policy, bootstrap, manifest, and existing runtime tests**

```powershell
pwsh -NoProfile -File tests/scripts/Inochi2dBootstrapPolicyTest.ps1
pwsh -NoProfile -File scripts/bootstrap_inochi2d.ps1 -Target windows-x64
pwsh -NoProfile -File scripts/verify_inochi2d_runtime.ps1 -RuntimeRoot build/inochi2d/prefix/windows-x64 -ExpectedTarget windows-x64
cmake --preset windows-debug -DCS_ENABLE_INOCHI2D=ON -DCS_INOCHI2D_ROOT="$PWD/build/inochi2d/prefix/windows-x64"
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "Inochi2d(RuntimeManifest|ModelRuntime|AvatarRenderer)Test" --output-on-failure
```

Expected: all pass and the verifier prints v0.8.7 with the library SHA-256.

- [ ] **Step 5: Commit runtime auditing**

```powershell
git add scripts/bootstrap_inochi2d.ps1 scripts/verify_inochi2d_runtime.ps1 src/avatar/inochi2d/Inochi2dRuntimeManifest.h src/avatar/inochi2d/Inochi2dRuntimeManifest.cpp tests/avatar/inochi2d/Inochi2dRuntimeManifestTest.cpp tests/scripts/Inochi2dBootstrapPolicyTest.ps1 CMakeLists.txt CMakePresets.json src/avatar/CMakeLists.txt tests/CMakeLists.txt legal/OSS_BOM.csv
git commit -m "build(avatar): pin verified Inochi2D runtime"
```

## Task 2: Define the shared compiler result and 2D module contract

**Files:**
- Create: `src/avatar/AvatarCompileTypes.h`
- Create: `src/avatar/IAvatarCompiler.h`
- Create: `schemas/inochi2d-module.schema.json`
- Create: `src/avatar_2d_adapter/CMakeLists.txt`
- Create: `src/avatar_2d_adapter/Inochi2dModuleManifest.h`
- Create: `src/avatar_2d_adapter/Inochi2dModuleManifest.cpp`
- Create: `src/avatar_2d_adapter/Inochi2dModuleManifestCodec.h`
- Create: `src/avatar_2d_adapter/Inochi2dModuleManifestCodec.cpp`
- Create: `tests/avatar_2d_adapter/Inochi2dModuleManifestTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:

```cpp
enum class AvatarQualityTier {
    DesktopHigh, DesktopBalanced, Mobile
};
struct AvatarRendererOptions final {
    std::uint32_t width;
    std::uint32_t height;
    AvatarQualityTier qualityTier;
    bool preferGpuTexture{true};
    bool allowCpuReadback{false};
};
struct AvatarCompileRequest final {
    const AvatarSpec& spec;
    const IAvatarCatalog& catalog;
    std::filesystem::path cacheRoot;
    std::uint32_t canvasWidth;
    std::uint32_t canvasHeight;
    AvatarQualityTier qualityTier;
};
struct CompiledAvatarArtifact final {
    AvatarRepresentation representation;
    AvatarModelDescriptor descriptor;
    std::filesystem::path modelPath;
    std::filesystem::path descriptorPath;
    std::string contentSha256;
    std::uint32_t canvasWidth;
    std::uint32_t canvasHeight;
    AvatarQualityTier qualityTier;
    bool wasRebuilt{false};
};
class IAvatarCompiler {
public:
    virtual ~IAvatarCompiler() = default;
    [[nodiscard]] virtual core::Result<CompiledAvatarArtifact> compile(
        const AvatarCompileRequest& request) = 0;
protected:
    IAvatarCompiler() = default;
};
```

- [ ] **Step 1: Write module compatibility tests**

```cpp
TEST(Inochi2dModuleManifestTest, RejectsWrongMasterRigAndExternalReferences) {
    auto draft = validHairModule();
    draft.masterRigId = "core.rig.quadruped";
    EXPECT_FALSE(Inochi2dModuleManifest::create(std::move(draft)).hasValue());
    draft = validHairModule();
    draft.nodeIds.push_back("other-module/node");
    EXPECT_FALSE(Inochi2dModuleManifest::create(std::move(draft)).hasValue());
}

TEST(Inochi2dModuleManifestTest, RequiresMasksForEveryColorChannel) {
    auto draft = validHairModule();
    draft.colorChannels[0].maskTexturePath.clear();
    EXPECT_FALSE(Inochi2dModuleManifest::create(std::move(draft)).hasValue());
}
```

- [ ] **Step 2: Run and prove module tests fail**

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R Inochi2dModuleManifestTest --output-on-failure
```

Expected: target or module manifest is missing.

- [ ] **Step 3: Implement the exact module fields**

`Inochi2dModuleManifest` contains:

- schema version `1`;
- module asset ID/version and master-rig asset ID/version;
- one `AvatarSlot`, one anchor node ID, and lexical layer order;
- module namespace equal to `<asset-id>@<version>`;
- node IDs, texture paths, parameter IDs, mask relationships, and physics groups;
- color channels `{name, sourceTexturePath, maskTexturePath, blendMode}`;
- parameter bindings in the existing `AvatarParameterBinding` form;
- maximum texture dimension and expanded bytes.

Factories reject duplicate IDs/paths, a node outside the module namespace,
an anchor not declared by the master rig, unsupported blend modes, parameter
ranges outside `[-2, 2]`, textures over 8192×8192, and modules for a different
rig family or slot. The JSON schema uses `additionalProperties: false`.

- [ ] **Step 4: Run module schema and value tests**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R Inochi2dModuleManifestTest --output-on-failure
```

Expected: valid module round-trip and every rejection path pass.

- [ ] **Step 5: Commit compiler and module contracts**

```powershell
git add src/avatar/AvatarCompileTypes.h src/avatar/IAvatarCompiler.h src/avatar/CMakeLists.txt schemas/inochi2d-module.schema.json src/avatar_2d_adapter/CMakeLists.txt src/avatar_2d_adapter/Inochi2dModuleManifest.h src/avatar_2d_adapter/Inochi2dModuleManifest.cpp src/avatar_2d_adapter/Inochi2dModuleManifestCodec.h src/avatar_2d_adapter/Inochi2dModuleManifestCodec.cpp tests/avatar_2d_adapter/Inochi2dModuleManifestTest.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): define modular Inochi2D asset contract"
```

## Task 3: Read and write bounded deterministic INP containers

**Files:**
- Create: `src/avatar_2d_adapter/InpContainer.h`
- Create: `src/avatar_2d_adapter/InpContainer.cpp`
- Create: `tests/avatar_2d_adapter/InpContainerTest.cpp`
- Modify: `src/avatar_2d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `InpContainer::read(path)` and `InpContainer::write(path, value)`.

- [ ] **Step 1: Write golden, truncation, overflow, and determinism tests**

```cpp
TEST(InpContainerTest, ReadsAndWritesBigEndianContainerDeterministically) {
    const auto source = minimalContainer();
    ASSERT_TRUE(InpContainer::write(firstPath(), source).hasValue());
    ASSERT_TRUE(InpContainer::write(secondPath(), source).hasValue());
    EXPECT_EQ(core::sha256File(firstPath()).value(),
              core::sha256File(secondPath()).value());
    EXPECT_EQ(InpContainer::read(firstPath()).value(), source);
}

TEST(InpContainerTest, RejectsTruncatedAndOversizedSectionsBeforeAllocation) {
    EXPECT_EQ(InpContainer::read(truncatedJsonFixture()).error().code(),
              core::ErrorCode::ParseFailure);
    EXPECT_EQ(InpContainer::read(declaredFourGiBFixture()).error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove container tests fail**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R InpContainerTest --output-on-failure
```

Expected: `InpContainer` is undefined.

- [ ] **Step 3: Implement the container with exact limits**

```cpp
enum class InpTextureEncoding : std::uint8_t { Png = 0, Tga = 1, Bc7 = 2 };
struct InpTextureBlob final {
    InpTextureEncoding encoding;
    std::vector<std::byte> bytes;
    friend bool operator==(const InpTextureBlob&, const InpTextureBlob&) = default;
};
struct InpExtensionBlob final {
    std::string name;
    std::vector<std::byte> bytes;
    friend bool operator==(const InpExtensionBlob&, const InpExtensionBlob&) = default;
};
struct InpDocument final {
    nlohmann::json puppet;
    std::vector<InpTextureBlob> textures;
    std::vector<InpExtensionBlob> extensions;
    friend bool operator==(const InpDocument&, const InpDocument&) = default;
};
```

Limits are 64 MiB JSON, 64 textures, 512 MiB total texture bytes, 64
extensions, 64 MiB total extension bytes. Read every big-endian length into
`uint64_t`, compare to remaining file bytes and limits, then narrow. Write
canonical JSON with sorted object keys, preserve declared texture order, sort
extensions by name, flush a sibling temporary file, and atomically replace.

- [ ] **Step 4: Run the container suite**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R InpContainerTest --output-on-failure
```

Expected: golden-byte comparison, malformed magic, invalid texture encoding,
truncation, oversized sections, and deterministic hash pass.

- [ ] **Step 5: Commit INP I/O**

```powershell
git add src/avatar_2d_adapter/InpContainer.h src/avatar_2d_adapter/InpContainer.cpp src/avatar_2d_adapter/CMakeLists.txt tests/avatar_2d_adapter/InpContainerTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): add bounded deterministic INP containers"
```

## Task 4: Bake multi-channel colors into real module textures

**Files:**
- Create: `src/avatar_2d_adapter/Inochi2dTextureBaker.h`
- Create: `src/avatar_2d_adapter/Inochi2dTextureBaker.cpp`
- Create: `tests/avatar_2d_adapter/Inochi2dTextureBakerTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/avatar_2d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `legal/OSS_BOM.csv`
- Modify: `legal/THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Produces:

```cpp
struct TextureBakeChannel final {
    std::span<const std::byte> maskPng;
    ColorRgba color;
    std::string blendMode;
};
class Inochi2dTextureBaker final {
public:
    [[nodiscard]] core::Result<std::vector<std::byte>> bakePng(
        std::span<const std::byte> sourcePng,
        std::span<const TextureBakeChannel> channels) const;
};
```

- [ ] **Step 1: Write exact pixel and malformed-image tests**

Use a 2×2 source and masks. Assert exact sRGB-to-linear blend results after
rounding back to 8-bit RGBA, preserved alpha, and deterministic PNG bytes.
Reject mismatched dimensions, over-8192 dimensions, more than eight channels,
invalid PNG, and non-finite colors.

- [ ] **Step 2: Run and prove texture tests fail**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R Inochi2dTextureBakerTest --output-on-failure
```

Expected: baker type is missing.

- [ ] **Step 3: Pin stb and implement linear-light masking**

Fetch the exact stb archive from:

```cmake
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/31c1ad37456438565541f4919958214b6e762fb4.tar.gz
  URL_HASH SHA256=e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319)
```

Compile `stb_image` and `stb_image_write` implementation in one private
translation unit. Decode signed/trusted package images to RGBA8, convert RGB to
linear float, apply channels in manifest order using `multiply`, `screen`, or
`replace`, preserve source alpha multiplied by mask alpha, convert to sRGB, and
write PNG with fixed compression settings.

Add the stb MIT/Public Domain notice to the BOM and third-party notices.

- [ ] **Step 4: Run texture and full 2D unit tests**

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R "Inochi2d(TextureBaker|ModuleManifest|InpContainer)Test" --output-on-failure
```

Expected: exact pixel fixtures and malformed inputs pass.

- [ ] **Step 5: Commit texture baking**

```powershell
git add CMakeLists.txt src/avatar_2d_adapter/Inochi2dTextureBaker.h src/avatar_2d_adapter/Inochi2dTextureBaker.cpp src/avatar_2d_adapter/CMakeLists.txt tests/avatar_2d_adapter/Inochi2dTextureBakerTest.cpp tests/CMakeLists.txt legal/OSS_BOM.csv legal/THIRD_PARTY_NOTICES.md
git commit -m "feat(avatar): bake layered 2d avatar colors"
```

## Task 5: Compose namespaced rig modules into one puppet

**Files:**
- Create: `src/avatar_2d_adapter/Inochi2dModuleComposer.h`
- Create: `src/avatar_2d_adapter/Inochi2dModuleComposer.cpp`
- Create: `tests/avatar_2d_adapter/Inochi2dModuleComposerTest.cpp`
- Modify: `src/avatar_2d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:

```cpp
struct Inochi2dModuleInput final {
    Inochi2dModuleManifest manifest;
    InpDocument document;
    std::map<std::string, ColorRgba> colors;
};
struct ComposedInochi2dPuppet final {
    InpDocument document;
    AvatarParameterMapper parameterMapper;
};
class Inochi2dModuleComposer final {
public:
    [[nodiscard]] core::Result<ComposedInochi2dPuppet> compose(
        const Inochi2dModuleInput& masterRig,
        std::span<const Inochi2dModuleInput> modules) const;
};
```

- [ ] **Step 1: Write node remap, layer, mask, and collision tests**

```cpp
TEST(Inochi2dModuleComposerTest, RemapsIdsTexturesMasksAndParameters) {
    const auto result = composer().compose(masterRig(), {hair(), eyes()});
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(hasNode(result.value(), "core.hair@1.0.0/front"));
    EXPECT_TRUE(maskTargetsNamespacedTexture(result.value(), "hair-mask"));
    EXPECT_TRUE(parameterExistsOnce(result.value(), "ParamEyeLOpen"));
}

TEST(Inochi2dModuleComposerTest, RejectsCollisionOrMissingAnchor) {
    EXPECT_EQ(composer().compose(masterRig(), {collidingHair()}).error().code(),
              core::ErrorCode::AlreadyExists);
    EXPECT_EQ(composer().compose(masterRig(), {missingAnchorModule()}).error().code(),
              core::ErrorCode::NotFound);
}
```

- [ ] **Step 2: Run and prove composition tests fail**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R Inochi2dModuleComposerTest --output-on-failure
```

Expected: composer type is missing.

- [ ] **Step 3: Implement deterministic structural merge**

For each module in ascending `(slot, assetId, version)` order:

1. validate its declared master rig and anchor;
2. prefix every node, automation, physics group, and module-local parameter ID;
3. rewrite all parent, child, mask, deform, texture, and automation references;
4. append baked textures and offset texture indices;
5. attach the module root under the declared anchor;
6. resolve layer order using master slot order plus manifest lexical order;
7. merge parameter bindings and reject duplicate model-parameter names;
8. reject dangling references by walking the final JSON graph.

Do not infer unknown Inochi2D node types. Preserve them only when every
reference is internal to the signed module and the declared required
Inochi2D version is `0.8.x`.

- [ ] **Step 4: Run composer and container tests**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R "Inochi2d(ModuleComposer|InpContainer|TextureBaker)Test" --output-on-failure
```

Expected: deterministic output hash, namespacing, ordering, and all dangling
reference failures pass.

- [ ] **Step 5: Commit module composition**

```powershell
git add src/avatar_2d_adapter/Inochi2dModuleComposer.h src/avatar_2d_adapter/Inochi2dModuleComposer.cpp src/avatar_2d_adapter/CMakeLists.txt tests/avatar_2d_adapter/Inochi2dModuleComposerTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): compose pre-rigged Inochi2D modules"
```

## Task 6: Compile and cache `AvatarSpec` as a real INP model

**Files:**
- Create: `src/avatar_2d_adapter/Inochi2dCompileCache.h`
- Create: `src/avatar_2d_adapter/Inochi2dCompileCache.cpp`
- Create: `src/avatar_2d_adapter/Inochi2dAvatarCompiler.h`
- Create: `src/avatar_2d_adapter/Inochi2dAvatarCompiler.cpp`
- Create: `tests/avatar_2d_adapter/Inochi2dAvatarCompilerTest.cpp`
- Modify: `src/avatar/AvatarModelDescriptor.h`
- Modify: `src/avatar/AvatarModelDescriptor.cpp`
- Modify: `src/avatar_2d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `Inochi2dAvatarCompiler : IAvatarCompiler`.

- [ ] **Step 1: Write cache, rights, and atomic failure tests**

```cpp
TEST(Inochi2dAvatarCompilerTest, ProducesLoadableArtifactAndStableCacheKey) {
    const auto first = compiler().compile(request());
    const auto second = compiler().compile(request());
    ASSERT_TRUE(first.hasValue()) << first.error().message();
    ASSERT_TRUE(second.hasValue()) << second.error().message();
    EXPECT_EQ(first.value().contentSha256, second.value().contentSha256);
    EXPECT_EQ(first.value().modelPath, second.value().modelPath);
    EXPECT_TRUE(InpContainer::read(first.value().modelPath).hasValue());
}

TEST(Inochi2dAvatarCompilerTest, FailedRecompileKeepsLastGoodArtifact) {
    const auto good = compiler().compile(request());
    ASSERT_TRUE(good.hasValue());
    breakSelectedModule();
    EXPECT_FALSE(compiler().compile(request()).hasValue());
    EXPECT_TRUE(std::filesystem::exists(good.value().modelPath));
}
```

- [ ] **Step 2: Run and prove compiler tests fail**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R Inochi2dAvatarCompilerTest --output-on-failure
```

Expected: compiler type is missing.

- [ ] **Step 3: Implement compile resolution and cache transaction**

Compile key input, with NUL separators, is:

```text
compiler-id=creator-studio-inochi2d-compiler
compiler-version=1
canonical AvatarSpec JSON
quality tier
canvas width and height
for each selected asset: asset ID, version, manifest SHA-256, every payload SHA-256
```

Write to `cacheRoot/inochi2d/<compile-sha256>/`:

```text
avatar.inp
avatar-model.json
compile-input.json
```

Compile in a sibling staging directory, verify the emitted INP by reading it
again, write `AvatarModelDescriptor` with renderer `inochi2d`, canvas size,
parameter bindings, compile hash, source asset hashes, and then atomically
rename. A valid existing cache entry is immutable and returned without rewrite.

- [ ] **Step 4: Run compiler and full 2D adapter tests**

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R "Inochi2d.*Test|InpContainerTest" --output-on-failure
```

Expected: stable cache key, corrupted cache rebuild, failure rollback, rights
lookup, slot compatibility, and loadable INP tests pass.

- [ ] **Step 5: Commit the compiler**

```powershell
git add src/avatar_2d_adapter/Inochi2dCompileCache.h src/avatar_2d_adapter/Inochi2dCompileCache.cpp src/avatar_2d_adapter/Inochi2dAvatarCompiler.h src/avatar_2d_adapter/Inochi2dAvatarCompiler.cpp src/avatar_2d_adapter/CMakeLists.txt src/avatar/AvatarModelDescriptor.h src/avatar/AvatarModelDescriptor.cpp tests/avatar_2d_adapter/Inochi2dAvatarCompilerTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): compile and cache modular Inochi2D avatars"
```

## Task 7: Make the runtime render masks, blend modes, expressions, and physics

**Files:**
- Modify: `src/avatar/AvatarSoftwareRasterizer.h`
- Modify: `src/avatar/AvatarSoftwareRasterizer.cpp`
- Modify: `src/avatar/inochi2d/Inochi2dModelRuntime.h`
- Modify: `src/avatar/inochi2d/Inochi2dModelRuntime.cpp`
- Modify: `src/avatar/inochi2d/Inochi2dAvatarRenderer.h`
- Modify: `src/avatar/inochi2d/Inochi2dAvatarRenderer.cpp`
- Modify: `tests/avatar/AvatarSoftwareRasterizerTest.cpp`
- Modify: `tests/avatar/inochi2d/Inochi2dModelRuntimeTest.cpp`
- Modify: `tests/avatar/inochi2d/Inochi2dAvatarRendererTest.cpp`

**Interfaces:**
- Extends `AvatarSoftwareRenderInput` with explicit `blendMode`, `maskMode`,
  `maskGroup`, and stable texture cache key.

- [ ] **Step 1: Add failing blend, mask, physics-time, and texture-cache tests**

Test normal, multiply, screen, add, destination-in, clip-to-lower, nested masks,
backward timestamps, delta capped at 1/15 second, and that identical SDK texture
objects are copied once per model rather than once per frame.

- [ ] **Step 2: Run and prove the render tests fail**

```powershell
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "(AvatarSoftwareRasterizer|Inochi2dModelRuntime|Inochi2dAvatarRenderer)Test" --output-on-failure
```

Expected: new blend/mask assertions fail against the current normal-alpha-only
batch compositor.

- [ ] **Step 3: Preserve the SDK draw-state semantics**

Map the Inochi2D v0.8.7 draw command state to a renderer-neutral enum. Reject an
unknown command type or an out-of-range mask group rather than treating it as
normal blending. Cache textures by SDK texture identity and invalidate the
cache only when the loaded puppet changes. Cap a single physics update delta at
`1.0F / 15.0F`; split larger monotonic deltas into fixed substeps so resume
after pause does not explode hair and cloth.

Implement premultiplied-alpha blend equations in linear color for the listed
modes and preserve transparent black outside the puppet.

- [ ] **Step 4: Run renderer and regression tests**

```powershell
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "(AvatarSoftwareRasterizer|Inochi2dModelRuntime|Inochi2dAvatarRenderer|AvatarRenderPipeline)Test" --output-on-failure
```

Expected: blend/mask golden pixels, physics stepping, cache reuse, and all
existing renderer tests pass.

- [ ] **Step 5: Commit real draw semantics**

```powershell
git add src/avatar/AvatarSoftwareRasterizer.h src/avatar/AvatarSoftwareRasterizer.cpp src/avatar/inochi2d/Inochi2dModelRuntime.h src/avatar/inochi2d/Inochi2dModelRuntime.cpp src/avatar/inochi2d/Inochi2dAvatarRenderer.h src/avatar/inochi2d/Inochi2dAvatarRenderer.cpp tests/avatar/AvatarSoftwareRasterizerTest.cpp tests/avatar/inochi2d/Inochi2dModelRuntimeTest.cpp tests/avatar/inochi2d/Inochi2dAvatarRendererTest.cpp
git commit -m "feat(avatar): preserve Inochi2D blend mask and physics behavior"
```

## Task 8: Prove production 2D creation with a licensed real rig

**Files:**
- Create: `tests/fixtures/avatar-2d/README.md`
- Create: `tests/acceptance/Production2dAvatarAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`
- Create: `legal/ASSET_BOM.csv`

**Interfaces:**
- Consumes: foundation catalog, 2D compiler, verified runtime, render pipeline.
- Produces: end-to-end 2D release gate.

- [ ] **Step 1: Add the failing end-to-end acceptance test**

```cpp
TEST(Production2dAvatarAcceptanceTest, CustomizesCompilesTracksAndRendersRealPuppet) {
    const auto spec = fixtureSpec()
        .withHair("core.hair.layered-bob")
        .withEyeColor("#7B61FF")
        .withOuterwear("core.outfit.modern-jacket")
        .withTail("core.animal.fox-tail")
        .build();
    const auto compiled = compiler().compile(requestFor(spec));
    ASSERT_TRUE(compiled.hasValue()) << compiled.error().message();
    auto renderer = openVerifiedRenderer(compiled.value());
    ASSERT_TRUE(renderer.hasValue()) << renderer.error().message();
    const auto neutral = renderer.value()->render(t0(), neutralParameters());
    const auto expressive = renderer.value()->render(t1(), smileAndTurn());
    ASSERT_TRUE(neutral.hasValue());
    ASSERT_TRUE(expressive.hasValue());
    EXPECT_NE(core::sha256(neutral.value().bytes()),
              core::sha256(expressive.value().bytes()));
    EXPECT_TRUE(alphaContainsForegroundAndTransparentBackground(expressive.value()));
}
```

- [ ] **Step 2: Run and prove the production gate fails**

```powershell
cmake --build --preset windows-debug --target cs_production_2d_avatar_acceptance_tests
ctest --test-dir build/windows-debug -R Production2dAvatarAcceptanceTest --output-on-failure
```

Expected: the licensed fixture package and acceptance target are absent.

- [ ] **Step 3: Add one final-quality acceptance rig and its rights record**

The fixture is not a colored rectangle or synthetic triangle. It contains a
human/kemonomimi master rig with front/side/back hair, both eyes, brows, five
mouth shapes, jacket layers, ears, tail, masks, at least three color channels,
and independent hair/ear/tail physics. `legal/ASSET_BOM.csv` records creator,
contract/license reference, allowed commercial broadcast, derivative character,
app bundle, test redistribution, attribution, asset hash, and review date.

The fixture may be used only after the rights record exists and the pack passes
the foundation signature validator.

- [ ] **Step 4: Run the complete 2D gate**

```powershell
cmake --preset windows-debug -DCS_ENABLE_AVATAR_PACKS=ON -DCS_SODIUM_ROOT="$PWD/build/sodium/prefix" -DCS_ENABLE_INOCHI2D=ON -DCS_INOCHI2D_ROOT="$PWD/build/inochi2d/prefix"
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug -R "Production2dAvatarAcceptanceTest|Inochi2d.*Test|AvatarSoftwareRasterizerTest" --output-on-failure
ctest --test-dir build/windows-debug --output-on-failure
git diff --check
```

Expected: real puppet differences match committed golden hashes within the
documented pixel tolerance, transparent output is valid, and the full suite
passes.

- [ ] **Step 5: Commit the production 2D gate**

```powershell
git add tests/fixtures/avatar-2d/README.md tests/acceptance/Production2dAvatarAcceptanceTest.cpp tests/CMakeLists.txt README.md legal/ASSET_BOM.csv
git commit -m "test(avatar): qualify real modular Inochi2D creation"
```

## Plan Completion Gate

- A signed selection of pre-rigged parts compiles into a deterministic real INP.
- Color masks, expressions, masks, blend modes, and physics change real pixels.
- The verified v0.8.7 runtime loads and renders the compiled puppet.
- Bad containers, references, assets, runtime binaries, and cache entries fail
  without destroying the last good model.
- A licensed final-quality rig, not a static image, passes end-to-end acceptance.
- The later workspace/output plan still owns GPU zero-copy and the 1080p60
  physical-device performance claim.
