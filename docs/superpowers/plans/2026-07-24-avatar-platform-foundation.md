# Avatar Platform Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the versioned avatar specification, signed asset-pack format, commercial-rights resolver, installed catalog, and crash-safe project persistence shared by the 2D and 3D avatar creators.

**Architecture:** Keep character and rights semantics in `cs_avatar`, with no Qt or engine types. Put ZIP and Ed25519 implementation details in a new Qt-free `cs_avatar_pack_adapter`; store project-local `AvatarSpec` files through `cs_project_store`. Every install is validate-then-rename, every output authorization is recalculated from installed manifests, and stored rights snapshots are audit records only.

**Tech Stack:** C++20, CMake 3.25+, nlohmann/json 3.11.3, json-schema-validator 2.4.0, miniz 3.1.2 (`77d0dce8627735138c51770d1799a1ef48f2117d`), libsodium 1.0.22 (`77e1ce5`), GoogleTest 1.15.2.

## Global Constraints

- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- `cs_avatar`, `cs_avatar_pack_adapter`, and `cs_project_store` remain Qt/FFmpeg/MLT-free.
- C++ stays at C++20; use `core::Result<T>` and typed identifiers.
- Keep `core::ErrorCode` as a broad transport category. Avatar validators return
  the closest existing category and attach a stable, namespaced avatar issue code
  plus localized message key for precise diagnostics.
- Asset packages use ZIP container files named `*.csavatarpack`, `manifest.json` at archive root, SHA-256 per payload, and detached Ed25519 signature `signature.ed25519`.
- ZIP install limits: 2 GiB total expanded bytes, 512 MiB per entry, 10,000 entries, no absolute paths, drive prefixes, `..`, symlinks, hard links, or duplicate normalized names.
- The catalog never treats an imported third-party model as commercially cleared without a trusted signed rights manifest.
- A stored rights snapshot never authorizes an operation; `AvatarLicenseResolver` recalculates from current manifests for every broadcast, package, and model export.
- Core launch assets require `commercialBroadcast`, `appBundle`, and `derivativeCharacter` grants. Unknown and non-commercial grants cannot ship in the core catalog.
- Existing untracked workspace files are unrelated and must not be staged.

---

## File Structure

```text
schemas/
  avatar-spec.schema.json
  avatar-asset.schema.json
cmake/
  AvatarSpecSchema.h.in
  AvatarAssetSchema.h.in
  FindSodium.cmake
scripts/
  bootstrap_sodium.ps1
  verify_sodium_runtime.ps1
src/avatar/
  AvatarIdentifiers.h
  AvatarTypes.h
  AvatarSpec.h/.cpp
  AvatarSpecCodec.h/.cpp
  AvatarAssetManifest.h/.cpp
  AvatarAssetManifestCodec.h/.cpp
  AvatarLicense.h/.cpp
  AvatarLicenseResolver.h/.cpp
  IAvatarCatalog.h
src/avatar_pack_adapter/
  CMakeLists.txt
  AvatarPackArchive.h/.cpp
  SodiumSignatureVerifier.h/.cpp
  AvatarPackValidator.h/.cpp
  FileAvatarCatalog.h/.cpp
src/project_store/
  IAvatarSpecStore.h
  AvatarSpecFileStore.h/.cpp
tests/avatar/
  AvatarSpecTest.cpp
  AvatarSpecCodecTest.cpp
  AvatarLicenseResolverTest.cpp
  AvatarAssetManifestCodecTest.cpp
tests/avatar_pack_adapter/
  AvatarPackValidatorTest.cpp
  FileAvatarCatalogTest.cpp
tests/project_store/
  AvatarSpecFileStoreTest.cpp
tests/acceptance/
  AvatarFoundationAcceptanceTest.cpp
```

## Task 1: Add typed avatar values and validated `AvatarSpec`

**Files:**
- Create: `src/avatar/AvatarIdentifiers.h`
- Create: `src/avatar/AvatarTypes.h`
- Create: `src/avatar/AvatarSpec.h`
- Create: `src/avatar/AvatarSpec.cpp`
- Create: `tests/avatar/AvatarSpecTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `AvatarId`, `AvatarAssetId`, `AvatarPackageId`, `AvatarRepresentation`, `RigFamily`, `AvatarSlot`, `AssetRef`, `NamedScalar`, `ColorRgba`, `MaterialOverride`, `AvatarSpec::create(...)`.
- Consumed by: every later task and all six later plans.

- [ ] **Step 1: Write the failing value-object tests**

```cpp
TEST(AvatarSpecTest, AcceptsDeterministicallyOrderedSlotsAndMorphs) {
    auto spec = AvatarSpec::create(validDraft());
    ASSERT_TRUE(spec.hasValue()) << spec.error().message();
    EXPECT_EQ(spec.value().schemaVersion(), 1);
    EXPECT_EQ(spec.value().rigFamily(), RigFamily::Humanoid);
    EXPECT_EQ(spec.value().slots().at(AvatarSlot::HairFront).assetId.value(),
              "core.hair.front.soft-bob");
}

TEST(AvatarSpecTest, RejectsNonFiniteOrOutOfRangeMorphs) {
    auto draft = validDraft();
    draft.bodyMorphs = {{"height", 1.01F}};
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              core::ErrorCode::InvalidArgument);
    draft = validDraft();
    draft.faceMorphs = {{"eye-width", std::numeric_limits<float>::quiet_NaN()}};
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(AvatarSpecTest, RejectsDuplicateNamedValuesAndIncompatibleRepresentation) {
    auto draft = validDraft();
    draft.faceMorphs = {{"eye-width", 0.2F}, {"eye-width", 0.3F}};
    EXPECT_FALSE(AvatarSpec::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.rigFamily = RigFamily::Quadruped;
    draft.preferredRepresentation = AvatarRepresentation::Vrm1;
    EXPECT_FALSE(AvatarSpec::create(std::move(draft)).hasValue());
}
```

- [ ] **Step 2: Run the focused test and prove it fails**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R AvatarSpecTest --output-on-failure
```

Expected: build fails because `avatar/AvatarSpec.h` does not exist.

- [ ] **Step 3: Add the exact public value contract**

```cpp
// src/avatar/AvatarIdentifiers.h
#pragma once
#include "domain/Identifiers.h"
namespace creator::avatar {
struct AvatarIdTag;
struct AvatarAssetIdTag;
struct AvatarPackageIdTag;
using AvatarId = domain::Identifier<AvatarIdTag>;
using AvatarAssetId = domain::Identifier<AvatarAssetIdTag>;
using AvatarPackageId = domain::Identifier<AvatarPackageIdTag>;
}
```

```cpp
// src/avatar/AvatarTypes.h
#pragma once
#include "avatar/AvatarIdentifiers.h"
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace creator::avatar {
enum class AvatarRepresentation { Inochi2d, Vrm1, GltfRig };
enum class RigFamily {
    Humanoid, Kemonomimi, AnthroBiped, Mascot, Quadruped, Avian
};
enum class AvatarSlot {
    Body, Head, Face, Skin, HairFront, HairSideLeft, HairSideRight,
    HairBack, HairTie, Brows, Eyes, Nose, Mouth, Teeth, EarLeft,
    EarRight, Muzzle, HornLeft, HornRight, WingLeft, WingRight, Tail,
    Inner, Top, Bottom, Outerwear, Hands, Footwear, Headwear,
    FaceAccessory, BodyAccessory
};
struct AssetRef final {
    AvatarAssetId assetId;
    std::string version;
    std::string variantId;
    friend bool operator==(const AssetRef&, const AssetRef&) = default;
};
struct NamedScalar final {
    std::string name;
    float value{};
    friend bool operator==(const NamedScalar&, const NamedScalar&) = default;
};
struct ColorRgba final {
    float red{}, green{}, blue{}, alpha{1.0F};
    friend bool operator==(const ColorRgba&, const ColorRgba&) = default;
};
struct MaterialOverride final {
    std::string channel;
    ColorRgba baseColor;
    float metallic{}, roughness{1.0F}, emission{}, opacity{1.0F};
    friend bool operator==(const MaterialOverride&, const MaterialOverride&) = default;
};
}
```

`AvatarSpec.h` must expose this constructor boundary, with no public default
constructor:

```cpp
struct AvatarSpecDraft final {
    AvatarId avatarId;
    std::string displayName;
    RigFamily rigFamily{RigFamily::Humanoid};
    std::string speciesFamily;
    std::string styleTheme;
    AvatarRepresentation preferredRepresentation{AvatarRepresentation::Inochi2d};
    std::vector<NamedScalar> bodyMorphs;
    std::vector<NamedScalar> faceMorphs;
    std::vector<NamedScalar> animalMorphs;
    std::map<AvatarSlot, AssetRef> slots;
    std::map<std::string, ColorRgba> palette;
    std::vector<MaterialOverride> materials;
    std::vector<NamedScalar> expressions;
    std::vector<NamedScalar> physics;
    std::string trackingProfileId;
};

class AvatarSpec final {
public:
    static constexpr std::int32_t kCurrentSchemaVersion = 1;
    [[nodiscard]] static core::Result<AvatarSpec> create(AvatarSpecDraft draft);
    [[nodiscard]] std::int32_t schemaVersion() const noexcept;
    [[nodiscard]] const AvatarId& avatarId() const noexcept;
    [[nodiscard]] const std::string& displayName() const noexcept;
    [[nodiscard]] RigFamily rigFamily() const noexcept;
    [[nodiscard]] AvatarRepresentation preferredRepresentation() const noexcept;
    [[nodiscard]] const std::map<AvatarSlot, AssetRef>& slots() const noexcept;
    [[nodiscard]] const AvatarSpecDraft& values() const noexcept;
private:
    explicit AvatarSpec(AvatarSpecDraft values);
    AvatarSpecDraft values_;
};
```

`AvatarSpec::create` validates all of the following before moving the draft:

- display name and all names are valid UTF-8 and contain 1–200 Unicode code points;
- semantic versions match `^[0-9]+\.[0-9]+\.[0-9]+$`;
- every scalar and material number is finite;
- morph, expression, and physics values are within `[-1, 1]`;
- colors and material channels are within `[0, 1]`;
- names are unique inside each vector;
- `Vrm1` is accepted only for `Humanoid`, `Kemonomimi`, and `AnthroBiped`;
- `GltfRig` is accepted for every family and required for `Quadruped` and `Avian`;
- at least `Body`, `Head`, `Eyes`, and `Mouth` slots exist.

- [ ] **Step 4: Build and run the focused tests**

Run the Step 2 commands. Expected: every `AvatarSpecTest.*` test passes.

- [ ] **Step 5: Commit the value contract**

```powershell
git add src/avatar/AvatarIdentifiers.h src/avatar/AvatarTypes.h src/avatar/AvatarSpec.h src/avatar/AvatarSpec.cpp src/avatar/CMakeLists.txt tests/avatar/AvatarSpecTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): add validated shared avatar specification"
```

## Task 2: Add canonical JSON schemas and codecs

**Files:**
- Create: `schemas/avatar-spec.schema.json`
- Create: `cmake/AvatarSpecSchema.h.in`
- Create: `src/avatar/AvatarSpecCodec.h`
- Create: `src/avatar/AvatarSpecCodec.cpp`
- Create: `tests/avatar/AvatarSpecCodecTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `AvatarSpec`, nlohmann/json, json-schema-validator.
- Produces: `AvatarSpecCodec::toJson`, `AvatarSpecCodec::fromJson`, `AvatarSpecCodec::load`, `AvatarSpecCodec::save`.

- [ ] **Step 1: Write round-trip, unknown-field, and traversal tests**

```cpp
TEST(AvatarSpecCodecTest, RoundTripIsCanonicalAndStable) {
    const auto first = AvatarSpecCodec{}.toJson(validSpec());
    const auto decoded = AvatarSpecCodec{}.fromJson(first);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(AvatarSpecCodec{}.toJson(decoded.value()), first);
}

TEST(AvatarSpecCodecTest, RejectsUnknownFieldAndFutureVersion) {
    auto json = AvatarSpecCodec{}.toJson(validSpec());
    json["surprise"] = true;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(json).error().code(),
              core::ErrorCode::ParseFailure);
    json.erase("surprise");
    json["schemaVersion"] = AvatarSpec::kCurrentSchemaVersion + 1;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(json).error().code(),
              core::ErrorCode::UnsupportedVersion);
}
```

- [ ] **Step 2: Run and prove the codec test fails**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R AvatarSpecCodecTest --output-on-failure
```

Expected: build fails because `AvatarSpecCodec` is undefined.

- [ ] **Step 3: Implement the schema and codec**

The schema root is Draft 7, `additionalProperties: false`, requires every
top-level `AvatarSpecDraft` field, constrains semantic versions with the same
regex as Task 1, and enumerates every `RigFamily`, `AvatarRepresentation`, and
`AvatarSlot` spelling. Use these persisted spellings:

```text
representation: inochi2d | vrm-1.0 | gltf-rig
rigFamily: humanoid | kemonomimi | anthro-biped | mascot | quadruped | avian
slots: body, head, face, skin, hair-front, hair-side-left, hair-side-right,
       hair-back, hair-tie, brows, eyes, nose, mouth, teeth, ear-left,
       ear-right, muzzle, horn-left, horn-right, wing-left, wing-right, tail,
       inner, top, bottom, outerwear, hands, footwear, headwear,
       face-accessory, body-accessory
```

Expose:

```cpp
class AvatarSpecCodec final {
public:
    [[nodiscard]] nlohmann::json toJson(const AvatarSpec& spec) const;
    [[nodiscard]] core::Result<AvatarSpec> fromJson(
        const nlohmann::json& json) const;
    [[nodiscard]] core::Result<AvatarSpec> load(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> save(
        const std::filesystem::path& path, const AvatarSpec& spec) const;
};
```

Serialization sorts named vectors by `name`, material overrides by `channel`,
palette object keys lexically, and relies on `std::map` for slot order. `save`
writes UTF-8 JSON with two-space indentation to a sibling temporary file,
flushes it, and atomically replaces the destination. `load` caps input at
8 MiB and maps malformed JSON to `ParseFailure`, future schema to
`UnsupportedVersion`, missing file to `NotFound`, and I/O failures to
`IoFailure`.

Embed `schemas/avatar-spec.schema.json` using the same configure-header pattern
as `ProjectSchema.h`; add json-schema-validator to `cs_avatar`.

- [ ] **Step 4: Run codec and full avatar tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "Avatar(Spec|SpecCodec)Test" --output-on-failure
```

Expected: round-trip, malformed input, unknown field, future version, missing
file, and canonical order tests pass.

- [ ] **Step 5: Commit the codec**

```powershell
git add schemas/avatar-spec.schema.json cmake/AvatarSpecSchema.h.in CMakeLists.txt src/avatar/AvatarSpecCodec.h src/avatar/AvatarSpecCodec.cpp src/avatar/CMakeLists.txt tests/avatar/AvatarSpecCodecTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): persist canonical avatar specs"
```

## Task 3: Model commercial rights and asset manifests

**Files:**
- Create: `schemas/avatar-asset.schema.json`
- Create: `cmake/AvatarAssetSchema.h.in`
- Create: `src/avatar/AvatarLicense.h`
- Create: `src/avatar/AvatarLicense.cpp`
- Create: `src/avatar/AvatarAssetManifest.h`
- Create: `src/avatar/AvatarAssetManifest.cpp`
- Create: `src/avatar/AvatarAssetManifestCodec.h`
- Create: `src/avatar/AvatarAssetManifestCodec.cpp`
- Create: `src/avatar/AvatarLicenseResolver.h`
- Create: `src/avatar/AvatarLicenseResolver.cpp`
- Create: `tests/avatar/AvatarAssetManifestCodecTest.cpp`
- Create: `tests/avatar/AvatarLicenseResolverTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `LicenseGrant`, `AvatarUseContext`, `AvatarRightsDecision`, `AvatarAssetManifest`, `AvatarLicenseResolver::resolve`.
- Consumed by: pack validation, catalog, broadcast, portable project, and model exporters.

- [ ] **Step 1: Write rights intersection tests**

```cpp
TEST(AvatarLicenseResolverTest, NamesEveryAssetBlockingModelExport) {
    const auto result = AvatarLicenseResolver{}.resolve(
        commercialCorporateExport(),
        {manifest("core.body", allowAll()),
         manifest("core.hair", withGrant(AvatarRight::ModelExport,
                                         GrantState::Denied))});
    EXPECT_FALSE(result.allowed);
    ASSERT_EQ(result.blockers.size(), 1U);
    EXPECT_EQ(result.blockers[0].assetId.value(), "core.hair");
    EXPECT_EQ(result.blockers[0].right, AvatarRight::ModelExport);
}

TEST(AvatarLicenseResolverTest, SnapshotNeverOverridesCurrentManifest) {
    auto context = commercialBroadcast();
    context.recordedSnapshot = snapshotAllowingAll();
    const auto result = AvatarLicenseResolver{}.resolve(
        context, {manifest("core.body",
                           withGrant(AvatarRight::CommercialBroadcast,
                                     GrantState::Denied))});
    EXPECT_FALSE(result.allowed);
}
```

- [ ] **Step 2: Run and prove rights tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "Avatar(AssetManifest|LicenseResolver)Test" --output-on-failure
```

Expected: build fails because `AvatarLicenseResolver.h` does not exist.

- [ ] **Step 3: Add the exact rights types**

```cpp
enum class AvatarRight {
    CommercialBroadcast, AppBundle, DerivativeCharacter, ModelExport,
    RawAssetRedistribution, PortableProject, Attribution
};
enum class GrantState { Allowed, Denied, Conditional, Unknown };
enum class UserKind { Individual, Corporation };
enum class UseKind {
    Preview, Broadcast, Record, AppBundle, DerivativeCharacter,
    ModelExport, PortableProject
};
struct LicenseGrant final {
    AvatarRight right;
    GrantState state{GrantState::Unknown};
    std::string condition;
};
struct AvatarUseContext final {
    UserKind userKind{UserKind::Individual};
    UseKind useKind{UseKind::Preview};
    bool monetized{false};
    std::string region;
    core::Utc evaluatedAt;
};
struct AvatarRightBlocker final {
    AvatarAssetId assetId;
    AvatarRight right;
    GrantState state;
    std::string reason;
};
struct AvatarRightsDecision final {
    bool allowed{false};
    bool attributionRequired{false};
    std::vector<std::string> attributionLines;
    std::vector<AvatarRightBlocker> blockers;
};
struct AvatarRightsMatrix final {
    std::map<UseKind, AvatarRightsDecision> byUse;

    [[nodiscard]] const AvatarRightsDecision& forUse(UseKind use) const;
};
class AvatarAssetManifest;
class AvatarLicenseResolver final {
public:
    [[nodiscard]] AvatarRightsDecision resolve(
        const AvatarUseContext& context,
        std::span<const AvatarAssetManifest> manifests) const;
    [[nodiscard]] AvatarRightsMatrix resolveAll(
        UserKind userKind,
        bool monetized,
        std::string_view region,
        core::Utc evaluatedAt,
        std::span<const AvatarAssetManifest> manifests) const;
};
```

`AvatarAssetManifest` contains package ID/version, asset ID/version, display
name, vendor, supported representations, supported rig families, allowed slots,
dependencies, payload hashes, performance metadata, source URI, license ID and
version, grants, attribution text, region allow-list, valid-from, and optional
valid-until. Its factory rejects:

- duplicate payload paths, hashes, slots, rigs, representations, or grants;
- a hash not exactly 64 lowercase hexadecimal characters;
- an asset with no representation, rig, slot, or payload;
- `Allowed` attribution with empty text;
- end time not later than start time;
- a core asset missing explicit non-conditional grants for
  `CommercialBroadcast`, `AppBundle`, and `DerivativeCharacter`.

`AvatarLicenseResolver::resolve` maps `UseKind` to required rights, evaluates
user kind, monetization, region and time conditions, returns every blocker in
asset-ID order, and never reads a stored snapshot.
`AvatarLicenseResolver::resolveAll` evaluates every `UseKind` into an
`AvatarRightsMatrix` for editor display without turning that matrix into an
authorization cache.

- [ ] **Step 4: Implement and run schema/rights tests**

Embed `avatar-asset.schema.json` like Task 2. Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "Avatar(AssetManifest|LicenseResolver)Test" --output-on-failure
```

Expected: canonical asset manifest round-trip and all rights matrix cases pass.

- [ ] **Step 5: Commit manifests and rights**

```powershell
git add schemas/avatar-asset.schema.json cmake/AvatarAssetSchema.h.in CMakeLists.txt src/avatar/AvatarLicense.h src/avatar/AvatarLicense.cpp src/avatar/AvatarAssetManifest.h src/avatar/AvatarAssetManifest.cpp src/avatar/AvatarAssetManifestCodec.h src/avatar/AvatarAssetManifestCodec.cpp src/avatar/AvatarLicenseResolver.h src/avatar/AvatarLicenseResolver.cpp src/avatar/CMakeLists.txt tests/avatar/AvatarAssetManifestCodecTest.cpp tests/avatar/AvatarLicenseResolverTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): resolve asset compatibility and commercial rights"
```

## Task 4: Pin ZIP and Ed25519 dependencies behind an audited adapter

**Files:**
- Create: `cmake/FindSodium.cmake`
- Create: `scripts/bootstrap_sodium.ps1`
- Create: `scripts/verify_sodium_runtime.ps1`
- Create: `src/avatar_pack_adapter/CMakeLists.txt`
- Create: `tests/scripts/AvatarPackBootstrapPolicyTest.ps1`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `legal/OSS_BOM.csv`

**Interfaces:**
- Produces: CMake targets `miniz` and `Sodium::Sodium`, option `CS_ENABLE_AVATAR_PACKS`, cache path `CS_SODIUM_ROOT`, library `cs_avatar_pack_adapter`.

- [ ] **Step 1: Add a failing bootstrap policy test**

Create `tests/scripts/AvatarPackBootstrapPolicyTest.ps1` that reads the root
CMake and both scripts and fails unless it finds all exact pins:

```powershell
$required = @{
  'miniz' = 'f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a'
  'libsodium' = '3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279'
}
foreach ($pair in $required.GetEnumerator()) {
  if ($text -notmatch [regex]::Escape($pair.Value)) {
    throw "$($pair.Key) SHA-256 is not pinned"
  }
}
```

- [ ] **Step 2: Run and prove the policy test fails**

Run:

```powershell
pwsh -NoProfile -File tests/scripts/AvatarPackBootstrapPolicyTest.ps1
```

Expected: FAIL with `miniz SHA-256 is not pinned`.

- [ ] **Step 3: Add exact dependency pins**

Use:

```cmake
option(CS_ENABLE_AVATAR_PACKS "Build signed avatar package support" OFF)
set(CS_SODIUM_ROOT "" CACHE PATH "Audited libsodium prefix")

if(CS_ENABLE_AVATAR_PACKS)
  if(NOT CS_SODIUM_ROOT)
    message(FATAL_ERROR "CS_ENABLE_AVATAR_PACKS requires CS_SODIUM_ROOT")
  endif()
  FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a)
  FetchContent_MakeAvailable(miniz)
  find_package(Sodium REQUIRED)
  add_subdirectory(src/avatar_pack_adapter)
endif()
```

`bootstrap_sodium.ps1` downloads
`libsodium-1.0.22-msvc.zip`, verifies
`3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279`,
extracts to `build/sodium/prefix`, and writes
`build/sodium/prefix/runtime-manifest.json` containing version, source URL,
archive hash, include path, library path, and every staged DLL hash.
`verify_sodium_runtime.ps1` rejects missing or extra files and any hash change.

Add BOM rows:

```csv
miniz,Read and write signed avatar ZIP packages,Static inside avatar-pack adapter,MIT,Static library,APPROVED,https://github.com/richgel999/miniz,"Pinned 3.1.2 archive SHA-256 f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a"
libsodium,Ed25519 verification for avatar packages,Dynamic library behind avatar-pack adapter,ISC,Dynamic library,APPROVED_WITH_OBLIGATIONS,https://github.com/jedisct1/libsodium,"Pinned 1.0.22 MSVC archive SHA-256 3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279"
```

- [ ] **Step 4: Verify dependency policy and enabled configure**

Run:

```powershell
pwsh -NoProfile -File tests/scripts/AvatarPackBootstrapPolicyTest.ps1
pwsh -NoProfile -File scripts/bootstrap_sodium.ps1
cmake --preset windows-debug -DCS_ENABLE_AVATAR_PACKS=ON -DCS_SODIUM_ROOT="$PWD/build/sodium/prefix"
cmake --build --preset windows-debug --target cs_avatar_pack_adapter
```

Expected: policy passes, archive hash is printed, CMake finds
`Sodium::Sodium`, and the empty adapter target builds.

- [ ] **Step 5: Commit audited dependency wiring**

```powershell
git add CMakeLists.txt CMakePresets.json cmake/FindSodium.cmake scripts/bootstrap_sodium.ps1 scripts/verify_sodium_runtime.ps1 src/avatar_pack_adapter/CMakeLists.txt tests/scripts/AvatarPackBootstrapPolicyTest.ps1 legal/OSS_BOM.csv
git commit -m "build(avatar): pin signed asset package dependencies"
```

## Task 5: Validate and extract signed `.csavatarpack` archives

**Files:**
- Create: `src/avatar_pack_adapter/AvatarPackArchive.h`
- Create: `src/avatar_pack_adapter/AvatarPackArchive.cpp`
- Create: `src/avatar_pack_adapter/SodiumSignatureVerifier.h`
- Create: `src/avatar_pack_adapter/SodiumSignatureVerifier.cpp`
- Create: `src/avatar_pack_adapter/AvatarPackValidator.h`
- Create: `src/avatar_pack_adapter/AvatarPackValidator.cpp`
- Create: `tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp`
- Modify: `src/avatar_pack_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: asset manifest codec, miniz, libsodium, trusted key map.
- Produces: `AvatarPackValidator::validateAndExtract`.

- [ ] **Step 1: Write malicious-archive and valid-signature tests**

```cpp
TEST(AvatarPackValidatorTest, RejectsTraversalDuplicateAndZipBombEntries) {
    EXPECT_EQ(validate(fixtureWithEntry("../escape")).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(fixtureWithNormalizedDuplicates()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(fixtureDeclaringExpandedBytes(
                  2ULL * 1024ULL * 1024ULL * 1024ULL + 1ULL)).error().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(AvatarPackValidatorTest, RejectsTamperAndUnknownSigningKey) {
    EXPECT_EQ(validate(tamperedFixture()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(fixtureSignedByUnknownKey()).error().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(AvatarPackValidatorTest, ExtractsOnlyAfterAllChecksPass) {
    const auto result = validate(validSignedFixture());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(std::filesystem::exists(
        result.value().stagingRoot / "manifest.json"));
    EXPECT_EQ(result.value().manifest.assetId().value(), "core.body.humanoid");
}
```

- [ ] **Step 2: Run and prove the validator tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_pack_tests
ctest --test-dir build/windows-debug -R AvatarPackValidatorTest --output-on-failure
```

Expected: build fails because `AvatarPackValidator` is undefined.

- [ ] **Step 3: Implement archive and signature boundaries**

```cpp
struct TrustedAvatarKey final {
    std::string keyId;
    std::array<std::byte, crypto_sign_PUBLICKEYBYTES> publicKey;
};
struct ValidatedAvatarPack final {
    AvatarAssetManifest manifest;
    std::filesystem::path stagingRoot;
};
class AvatarPackValidator final {
public:
    AvatarPackValidator(std::vector<TrustedAvatarKey> trustedKeys,
                        std::filesystem::path stagingParent);
    [[nodiscard]] core::Result<ValidatedAvatarPack> validateAndExtract(
        const std::filesystem::path& packagePath) const;
};
```

Validation order is fixed:

1. Open without extraction and enforce entry/count/size/path rules.
2. Read `manifest.json`, `signature.ed25519`, and `signing-key-id.txt` into
   bounded memory.
3. Validate manifest schema and locate the trusted public key.
4. Build the signature message as canonical manifest bytes followed by each
   payload path, NUL, raw 32-byte SHA-256, in lexical path order.
5. Verify Ed25519 with `crypto_sign_verify_detached`.
6. Stream each payload to a newly created unpredictable staging directory,
   hash while writing, flush, compare, and delete staging on any mismatch.
7. Return the staging path only after every entry passes.

Never log signature bytes, model contents, or extracted user paths.

- [ ] **Step 4: Run validator tests and a sanitizer/fuzz seed loop**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_pack_tests
ctest --test-dir build/windows-debug -R AvatarPackValidatorTest --output-on-failure
build/windows-debug/tests/cs_avatar_pack_tests.exe --gtest_filter=AvatarPackValidatorFuzzSeedTest.*
```

Expected: traversal, duplicate, truncated ZIP, CRC mismatch, signature mismatch,
unknown key, oversize entry, and valid fixture cases pass.

- [ ] **Step 5: Commit the secure pack reader**

```powershell
git add src/avatar_pack_adapter/AvatarPackArchive.h src/avatar_pack_adapter/AvatarPackArchive.cpp src/avatar_pack_adapter/SodiumSignatureVerifier.h src/avatar_pack_adapter/SodiumSignatureVerifier.cpp src/avatar_pack_adapter/AvatarPackValidator.h src/avatar_pack_adapter/AvatarPackValidator.cpp src/avatar_pack_adapter/CMakeLists.txt tests/avatar_pack_adapter/AvatarPackValidatorTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): verify and extract signed avatar packs"
```

## Task 6: Add transactional installed catalog

**Files:**
- Create: `src/avatar/IAvatarCatalog.h`
- Create: `src/avatar_pack_adapter/FileAvatarCatalog.h`
- Create: `src/avatar_pack_adapter/FileAvatarCatalog.cpp`
- Create: `tests/avatar_pack_adapter/FileAvatarCatalogTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `src/avatar_pack_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:

```cpp
class IAvatarCatalog {
public:
    virtual ~IAvatarCatalog() = default;
    [[nodiscard]] virtual core::Result<std::vector<AvatarAssetManifest>>
        list() const = 0;
    [[nodiscard]] virtual core::Result<AvatarAssetManifest> find(
        const AvatarAssetId& id, std::string_view version) const = 0;
    [[nodiscard]] virtual core::Result<std::filesystem::path> payloadRoot(
        const AvatarAssetId& id, std::string_view version) const = 0;
protected:
    IAvatarCatalog() = default;
};
```

- [ ] **Step 1: Write install, rollback, and tamper tests**

```cpp
TEST(FileAvatarCatalogTest, InstallBecomesVisibleOnlyAfterAtomicRename) {
    FileAvatarCatalog catalog{catalogRoot(), validator()};
    ASSERT_TRUE(catalog.install(validPack()).hasValue());
    ASSERT_EQ(catalog.list().value().size(), 1U);
    EXPECT_EQ(catalog.list().value()[0].assetId().value(), "core.body.humanoid");
}

TEST(FileAvatarCatalogTest, FailedUpgradeLeavesPreviousVersionUsable) {
    FileAvatarCatalog catalog{catalogRoot(), validator()};
    ASSERT_TRUE(catalog.install(validPack("1.0.0")).hasValue());
    EXPECT_FALSE(catalog.install(tamperedPack("1.1.0")).hasValue());
    EXPECT_TRUE(catalog.find(assetId(), "1.0.0").hasValue());
}

TEST(FileAvatarCatalogTest, RechecksManifestHashBeforeReturningPayload) {
    FileAvatarCatalog catalog{catalogRoot(), validator()};
    ASSERT_TRUE(catalog.install(validPack()).hasValue());
    tamperInstalledPayload();
    EXPECT_EQ(catalog.payloadRoot(assetId(), "1.0.0").error().code(),
              core::ErrorCode::IoFailure);
}
```

- [ ] **Step 2: Run and prove catalog tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_pack_tests
ctest --test-dir build/windows-debug -R FileAvatarCatalogTest --output-on-failure
```

Expected: build fails because `FileAvatarCatalog.h` does not exist.

- [ ] **Step 3: Implement the on-disk transaction**

Use layout:

```text
catalog-root/
  installed/<package-id>/<package-version>/
    manifest.json
    signature.ed25519
    payload/...
  quarantine/<uuid>/
  staging/<uuid>/
```

`install` obtains a process-local mutex and an exclusive
`catalog-root/catalog.lock`, calls `validateAndExtract`, fsyncs the staging tree,
renames it to the final version directory, and fsyncs its parent. Existing
versions are immutable. A conflicting package ID/version with a different
manifest hash returns `AlreadyExists`. Startup removes abandoned staging
directories older than 24 hours and never deletes quarantine automatically.

- [ ] **Step 4: Run catalog and pack tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_pack_tests
ctest --test-dir build/windows-debug -R "(AvatarPackValidator|FileAvatarCatalog)Test" --output-on-failure
```

Expected: all transaction and tamper tests pass with no leaked staging folder.

- [ ] **Step 5: Commit the catalog**

```powershell
git add src/avatar/IAvatarCatalog.h src/avatar/CMakeLists.txt src/avatar_pack_adapter/FileAvatarCatalog.h src/avatar_pack_adapter/FileAvatarCatalog.cpp src/avatar_pack_adapter/CMakeLists.txt tests/avatar_pack_adapter/FileAvatarCatalogTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): install immutable avatar asset catalog"
```

## Task 7: Persist avatar specs crash-safely inside projects

**Files:**
- Create: `src/project_store/IAvatarSpecStore.h`
- Create: `src/project_store/AvatarSpecFileStore.h`
- Create: `src/project_store/AvatarSpecFileStore.cpp`
- Create: `tests/project_store/AvatarSpecFileStoreTest.cpp`
- Modify: `src/domain/ProjectManifest.h`
- Modify: `src/domain/ProjectManifest.cpp`
- Modify: `schemas/project.schema.json`
- Modify: `src/project_store/JsonProjectStore.cpp`
- Modify: `src/project_store/ProjectPackageStore.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `tests/domain/ProjectManifestTest.cpp`
- Modify: `tests/project_store/JsonProjectStoreTest.cpp`
- Modify: `tests/project_store/ProjectPackageStoreTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: project `avatars` directory and `IAvatarSpecStore`.

- [ ] **Step 1: Write backward-compatibility and recovery tests**

```cpp
TEST(AvatarSpecFileStoreTest, SavesLoadsAndKeepsLastGoodCopy) {
    AvatarSpecFileStore store{projectRoot() / "avatars"};
    ASSERT_TRUE(store.save(validSpec()).hasValue());
    EXPECT_EQ(store.load(validSpec().avatarId()).value().values(),
              validSpec().values());
    corruptPrimarySpec();
    EXPECT_EQ(store.load(validSpec().avatarId()).value().values(),
              validSpec().values());
}

TEST(JsonProjectStoreTest, OlderManifestWithoutAvatarsUsesDefaultDirectory) {
    writeManifestWithoutAvatars();
    const auto loaded = JsonProjectStore{}.load(packageRoot());
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value().directories.avatars, "avatars");
}
```

- [ ] **Step 2: Run and prove persistence tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "(AvatarSpecFileStore|JsonProjectStore|ProjectManifest)Test" --output-on-failure
```

Expected: build fails because `ProjectDirectories::avatars` and
`AvatarSpecFileStore` do not exist.

- [ ] **Step 3: Add the backward-compatible project directory**

Add `std::string avatars{"avatars"};` to `ProjectDirectories`.
`schemas/project.schema.json` permits `directories.avatars` but does not add it
to the required list, so schema-version 1 projects remain valid.
`JsonProjectStore::load` supplies `"avatars"` when absent; `save` always writes
it. `ProjectPackageStore::create` creates and verifies the directory with the
same no-symlink policy as the existing package directories.

Expose:

```cpp
class IAvatarSpecStore {
public:
    virtual ~IAvatarSpecStore() = default;
    [[nodiscard]] virtual core::Result<void> save(const AvatarSpec& spec) = 0;
    [[nodiscard]] virtual core::Result<AvatarSpec> load(
        const AvatarId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<AvatarId>> list() const = 0;
protected:
    IAvatarSpecStore() = default;
};
```

Store each character as:

```text
avatars/<avatar-id>/avatar.json
avatars/<avatar-id>/avatar.last-good.json
avatars/<avatar-id>/autosave.json
```

Use `internal::DurableFile` for atomic replacement. Validate directory
containment before every read/write, cap JSON at 8 MiB, and use
`avatar.last-good.json` only when the primary fails schema or I/O validation.

- [ ] **Step 4: Run project and avatar persistence tests**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
ctest --test-dir build/windows-debug -R "(AvatarSpecFileStore|JsonProjectStore|ProjectPackageStore|ProjectManifest)Test" --output-on-failure
```

Expected: new and old manifests open, paths cannot escape, and last-good
recovery passes.

- [ ] **Step 5: Commit project persistence**

```powershell
git add src/project_store/IAvatarSpecStore.h src/project_store/AvatarSpecFileStore.h src/project_store/AvatarSpecFileStore.cpp src/project_store/CMakeLists.txt src/domain/ProjectManifest.h src/domain/ProjectManifest.cpp schemas/project.schema.json src/project_store/JsonProjectStore.cpp src/project_store/ProjectPackageStore.cpp tests/project_store/AvatarSpecFileStoreTest.cpp tests/domain/ProjectManifestTest.cpp tests/project_store/JsonProjectStoreTest.cpp tests/project_store/ProjectPackageStoreTest.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): persist avatar specs in project packages"
```

## Task 8: Prove the foundation with a real signed pack acceptance path

**Files:**
- Create: `tests/fixtures/avatar-packs/README.md`
- Create: `tests/acceptance/AvatarFoundationAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`
- Create: `legal/THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Consumes: all tasks in this plan.
- Produces: one end-to-end gate proving signed install, rights evaluation,
  spec save/reopen, and tamper rejection.

- [ ] **Step 1: Add the failing acceptance test**

```cpp
TEST(AvatarFoundationAcceptanceTest, SignedCommercialAssetSurvivesProjectRoundTrip) {
    TestSignedPackFactory factory;
    const auto pack = factory.createCommercialPack();
    FileAvatarCatalog catalog{catalogRoot(), factory.validator()};
    ASSERT_TRUE(catalog.install(pack).hasValue());

    AvatarSpecFileStore specs{projectRoot() / "avatars"};
    ASSERT_TRUE(specs.save(specUsingInstalledAsset()).hasValue());
    const auto reopened = specs.load(specUsingInstalledAsset().avatarId());
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();

    const auto manifest = catalog.find(assetId(), "1.0.0");
    ASSERT_TRUE(manifest.hasValue());
    const auto rights = AvatarLicenseResolver{}.resolve(
        corporateMonetizedBroadcast(), {manifest.value()});
    EXPECT_TRUE(rights.allowed);

    tamperInstalledPayload();
    EXPECT_FALSE(catalog.payloadRoot(assetId(), "1.0.0").hasValue());
}
```

- [ ] **Step 2: Run and prove the acceptance target fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_foundation_acceptance_tests
ctest --test-dir build/windows-debug -R AvatarFoundationAcceptanceTest --output-on-failure
```

Expected: target or fixture factory is missing.

- [ ] **Step 3: Add deterministic test signing and documentation**

The fixture factory creates source files at test runtime, signs them with a
test-only key compiled only into the acceptance binary, and writes the pack
with miniz. No private key or prebuilt binary enters the shipped target.

Document:

- how to enable `CS_ENABLE_AVATAR_PACKS`;
- the catalog and project paths;
- what `상업 사용 확인됨` means;
- why imported models remain user-confirmed;
- miniz and libsodium notices and source-offer locations.

- [ ] **Step 4: Run the complete foundation gate**

Run:

```powershell
cmake --preset windows-debug -DCS_ENABLE_AVATAR_PACKS=ON -DCS_SODIUM_ROOT="$PWD/build/sodium/prefix"
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug -R "Avatar(Spec|Asset|License|Pack|Catalog|Foundation)" --output-on-failure
ctest --test-dir build/windows-debug --output-on-failure
git diff --check
```

Expected: focused and full suites pass; no warning or whitespace error.

- [ ] **Step 5: Commit the foundation acceptance gate**

```powershell
git add tests/fixtures/avatar-packs/README.md tests/acceptance/AvatarFoundationAcceptanceTest.cpp tests/CMakeLists.txt README.md legal/THIRD_PARTY_NOTICES.md
git commit -m "test(avatar): prove signed commercial avatar foundation"
```

## Plan Completion Gate

- `AvatarSpec` and manifests are schema-validated, canonical, and migration-safe.
- Signed package validation rejects every listed archive and signature attack.
- Catalog installation is immutable, atomic, and tamper-detecting.
- Rights are recalculated from current manifests and identify every blocker.
- Old projects without an `avatars` directory entry still open.
- The end-to-end signed pack acceptance test and full existing suite pass.
- Only after this gate may the 2D, 3D, animal, tracking, workspace, and content
  plans consume these interfaces.
