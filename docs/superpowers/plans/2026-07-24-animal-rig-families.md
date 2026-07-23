# Animal Rig Families Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make kemonomimi, anthro biped, mascot, quadruped, and avian avatars first-class customizable rigs in both 2D and 3D, with family-correct expressions, body controls, secondary motion, part compatibility, and truthful runtime metadata.

**Architecture:** A Qt-free `RigDefinition` registry declares versioned semantic bones, deformers, expressions, targets, and secondary-motion chains for every family. Representation adapters bind the same semantics to Inochi2D parameters or VRM/glTF nodes without pretending the structures are identical. A data-driven `RigRetargetProfile` describes how canonical face/body/hand motion drives ears, muzzle, paws, wings, tail, and idle behavior; the later tracking plan executes those profiles. Compatibility validators reject cross-family or out-of-range parts before either compiler mutates a model.

**Tech Stack:** C++20, nlohmann/json 3.11.3, JSON Schema Draft 7, Inochi2D adapter from the 2D plan, VRM/glTF adapter from the 3D plan, GoogleTest 1.15.2.

## Global Constraints

- Requires completion of `2026-07-24-avatar-platform-foundation.md`, `2026-07-24-production-2d-avatar-creator.md`, and `2026-07-24-production-3d-avatar-creator.md`.
- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- The five animal families are `RigFamily::Kemonomimi`,
  `RigFamily::AnthroBiped`, `RigFamily::Mascot`,
  `RigFamily::Quadruped`, and `RigFamily::Avian`.
- Every family must have a real 2D definition and a real 3D definition. A
  human head with decorative ears does not qualify for anthro, mascot,
  quadruped, or avian acceptance.
- Kemonomimi and compatible anthro bipeds may use VRM 1.0 only when they retain
  the complete humanoid bone map. Mascot, quadruped, and avian 3D rigs use
  `AvatarRepresentation::GltfRig` and `CS_avatar_rig`.
- 2D rigs use semantic deformers and parameters; 3D rigs use bones, morphs, and
  materials. The registry shares meanings, not engine object IDs.
- Family definitions are versioned immutable data. A breaking required-control
  change increments the major version; an additive optional control increments
  the minor version.
- Missing required bones, deformers, parameters, expressions, or physics chains
  fail compilation. Optional controls have an explicit profile fallback.
- Animal expression mappings are authored per family. Human eye, mouth, and
  hand values are never copied blindly to muzzle, beak, paw, or wing controls.
- Secondary motion uses deterministic fixed-step simulation under replay tests.
- Asset compatibility is declared, measurable, and testable; the compiler never
  shrinks, hides, or drops an incompatible part to make the build succeed.
- All fixture art and rigs require the same signed-package and commercial-rights
  evidence as human assets.

---

## File Structure

```text
src/avatar/
  RigSemantics.h
  RigDefinition.h/.cpp
  RigDefinitionCodec.h/.cpp
  RigDefinitionRegistry.h/.cpp
  RigRetargetProfile.h/.cpp
  RigRetargetProfileCodec.h/.cpp
  RigCompatibilityValidator.h/.cpp
  SecondaryMotionDefinition.h
src/avatar_2d_adapter/
  Inochi2dRigBinding.h/.cpp
  Inochi2dAnimalRigValidator.h/.cpp
src/avatar_3d_adapter/
  Avatar3dRigBinding.h/.cpp
  Avatar3dAnimalRigValidator.h/.cpp
src/avatar_motion/
  CMakeLists.txt
  AnimalSecondaryMotion.h/.cpp
rigs/
  v1/
    kemonomimi-2d.json
    kemonomimi-3d.json
    anthro-biped-2d.json
    anthro-biped-3d.json
    mascot-2d.json
    mascot-3d.json
    quadruped-2d.json
    quadruped-3d.json
    avian-2d.json
    avian-3d.json
    retarget-kemonomimi.json
    retarget-anthro-biped.json
    retarget-mascot.json
    retarget-quadruped.json
    retarget-avian.json
schemas/
  rig-definition.schema.json
  rig-retarget-profile.schema.json
tests/avatar/
  RigDefinitionCodecTest.cpp
  RigDefinitionRegistryTest.cpp
  RigRetargetProfileTest.cpp
  RigCompatibilityValidatorTest.cpp
tests/avatar_2d_adapter/
  Inochi2dAnimalRigValidatorTest.cpp
tests/avatar_3d_adapter/
  Avatar3dAnimalRigValidatorTest.cpp
tests/avatar_motion/
  AnimalSecondaryMotionTest.cpp
tests/acceptance/
  AnimalRigFamiliesAcceptanceTest.cpp
```

## Task 1: Define stable cross-representation rig semantics

**Files:**
- Create: `src/avatar/RigSemantics.h`
- Create: `src/avatar/SecondaryMotionDefinition.h`
- Create: `src/avatar/RigDefinition.h`
- Create: `src/avatar/RigDefinition.cpp`
- Create: `src/avatar/RigDefinitionCodec.h`
- Create: `src/avatar/RigDefinitionCodec.cpp`
- Create: `schemas/rig-definition.schema.json`
- Create: `tests/avatar/RigDefinitionCodecTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class RigElementKind { Bone, Deformer, Parameter, Morph };

enum class RigSemantic {
    Root, Center, Pelvis, SpineLower, SpineUpper, Chest, Neck, Head,
    EyeLeft, EyeRight, Jaw, Muzzle, Nose, BeakUpper, BeakLower,
    EarLeftBase, EarLeftTip, EarRightBase, EarRightTip,
    ShoulderLeft, UpperArmLeft, LowerArmLeft, HandLeft,
    ShoulderRight, UpperArmRight, LowerArmRight, HandRight,
    WingLeftBase, WingLeftMid, WingLeftTip,
    WingRightBase, WingRightMid, WingRightTip,
    UpperLegLeft, LowerLegLeft, FootLeft, ToeLeft,
    UpperLegRight, LowerLegRight, FootRight, ToeRight,
    FrontLegLeftUpper, FrontLegLeftLower, FrontPawLeft,
    FrontLegRightUpper, FrontLegRightLower, FrontPawRight,
    HindLegLeftUpper, HindLegLeftLower, HindPawLeft,
    HindLegRightUpper, HindLegRightLower, HindPawRight,
    TailBase, TailMid, TailTip, TailFan
};

enum class RigExpressionSemantic {
    BlinkLeft, BlinkRight, LookLeft, LookRight, LookUp, LookDown,
    MouthA, MouthI, MouthU, MouthE, MouthO, Smile, Frown, Surprise,
    BrowUpLeft, BrowUpRight, BrowDownLeft, BrowDownRight,
    MuzzleRaise, MuzzlePuff, NoseWrinkle,
    BeakOpen, BeakSmile, EarAlert, EarRelaxed
};

enum class RigTargetSemantic {
    Gaze, Head, Chest, Pelvis, HandLeft, HandRight,
    FrontPawLeft, FrontPawRight, WingLeft, WingRight, Ground
};

struct RigElementBinding {
    RigSemantic semantic;
    RigElementKind kind;
    std::string runtimeName;
    bool required;
};

struct RigExpressionBinding {
    RigExpressionSemantic semantic;
    std::string runtimeName;
    float minimum;
    float maximum;
    bool required;
};

struct SecondaryMotionChainDefinition {
    std::string id;
    std::vector<RigSemantic> elements;
    float stiffness;
    float damping;
    float gravity;
    float maxAngleRadians;
};

struct RigDefinition {
    std::uint32_t schemaVersion;
    std::string id;
    std::string version;
    RigFamily family;
    AvatarRepresentation representation;
    std::string coordinateSystem;
    float metersPerUnit;
    std::vector<RigElementBinding> elements;
    std::vector<RigExpressionBinding> expressions;
    std::map<RigTargetSemantic, std::string> targets;
    std::vector<SecondaryMotionChainDefinition> secondaryMotion;
};

class RigDefinitionCodec final {
public:
    static core::Result<RigDefinition> decode(const nlohmann::json& json);
    static nlohmann::json encode(const RigDefinition& definition);
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing strict-codec tests**

```cpp
TEST(RigDefinitionCodecTest, RoundTripsACompleteQuadrupedDefinition) {
    const auto decoded = RigDefinitionCodec::decode(loadFixture("quadruped-3d"));
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(decoded.value().family, RigFamily::Quadruped);
    EXPECT_EQ(decoded.value().representation, AvatarRepresentation::GltfRig);
    EXPECT_TRUE(hasRequired(decoded.value(), RigSemantic::FrontPawLeft));
    EXPECT_EQ(RigDefinitionCodec::encode(decoded.value()),
              canonicalFixture("quadruped-3d"));
}

TEST(RigDefinitionCodecTest, RejectsDuplicateOutOfRangeAndUnknownValues) {
    EXPECT_EQ(decode(definitionWithDuplicate(RigSemantic::Head)).error().code(),
              core::ErrorCode::ParseFailure);
    EXPECT_EQ(decode(definitionWithMetersPerUnit(0.0F)).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(decode(definitionWithUnknownSemantic("flipper-left")).error().code(),
              core::ErrorCode::ParseFailure);
}
```

- [ ] **Step 2: Run and prove the rig codec tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R RigDefinitionCodecTest --output-on-failure
```

Expected: the semantic and codec types do not exist.

- [ ] **Step 3: Implement closed enums and schema validation**

`rig-definition.schema.json` uses Draft 7, requires all root properties, limits
IDs and runtime names to 128 UTF-8 bytes, validates semantic versions, bounds
meters-per-unit to `[0.001, 1000]`, bounds motion coefficients, and sets
`additionalProperties: false` at every object level. Persisted enum spellings
are kebab-case equivalents of the C++ enum names.

The codec validates the schema before materializing a C++ value, rejects
duplicate semantic/runtime names, requires finite floating-point values, sorts
encoded maps and arrays by semantic value, and produces byte-stable canonical
JSON. It does not infer a family from filenames or runtime node names.

- [ ] **Step 4: Run codec, round-trip, and malformed-schema tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R RigDefinitionCodecTest --output-on-failure
```

Expected: valid definitions round-trip byte-for-byte; unknown fields, invalid
enums, duplicates, non-finite numbers, and excessive strings fail.

- [ ] **Step 5: Commit stable rig semantics**

```powershell
git add src/avatar/RigSemantics.h src/avatar/SecondaryMotionDefinition.h src/avatar/RigDefinition.h src/avatar/RigDefinition.cpp src/avatar/RigDefinitionCodec.h src/avatar/RigDefinitionCodec.cpp schemas/rig-definition.schema.json tests/avatar/RigDefinitionCodecTest.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): define animal rig semantics"
```

## Task 2: Author and validate all ten family/representation definitions

**Files:**
- Create: `src/avatar/RigDefinitionRegistry.h`
- Create: `src/avatar/RigDefinitionRegistry.cpp`
- Create: `rigs/v1/kemonomimi-2d.json`
- Create: `rigs/v1/kemonomimi-3d.json`
- Create: `rigs/v1/anthro-biped-2d.json`
- Create: `rigs/v1/anthro-biped-3d.json`
- Create: `rigs/v1/mascot-2d.json`
- Create: `rigs/v1/mascot-3d.json`
- Create: `rigs/v1/quadruped-2d.json`
- Create: `rigs/v1/quadruped-3d.json`
- Create: `rigs/v1/avian-2d.json`
- Create: `rigs/v1/avian-3d.json`
- Create: `tests/avatar/RigDefinitionRegistryTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

class RigDefinitionRegistry final {
public:
    core::Result<void> loadDirectory(const std::filesystem::path& root);
    core::Result<std::reference_wrapper<const RigDefinition>> require(
        RigFamily family,
        AvatarRepresentation representation,
        std::string_view version) const;
    std::vector<std::reference_wrapper<const RigDefinition>> list() const;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add the failing registry coverage tests**

```cpp
TEST(RigDefinitionRegistryTest, ContainsEveryAnimalFamilyIn2dAnd3d) {
    auto registry = loadBundledRegistry();
    for (const auto family : {RigFamily::Kemonomimi, RigFamily::AnthroBiped,
                              RigFamily::Mascot, RigFamily::Quadruped,
                              RigFamily::Avian}) {
        EXPECT_TRUE(registry.require(family, AvatarRepresentation::Inochi2d,
                                     "1.0.0").hasValue());
        const auto representation =
            family == RigFamily::Kemonomimi ||
                    family == RigFamily::AnthroBiped
                ? AvatarRepresentation::Vrm1
                : AvatarRepresentation::GltfRig;
        EXPECT_TRUE(registry.require(family, representation, "1.0.0").hasValue());
    }
}

TEST(RigDefinitionRegistryTest, RejectsAFamilyMissingItsRequiredSemantics) {
    EXPECT_EQ(loadRegistryWithChangedDefinition(
                  "avian-3d", remove(RigSemantic::WingLeftTip)).error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove definition coverage fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R RigDefinitionRegistryTest --output-on-failure
```

Expected: the registry and ten bundled definitions are absent.

- [ ] **Step 3: Implement exact family requirements**

The registry applies these minimum required semantic sets to both
representations, using deformers/parameters in 2D and bones/morphs in 3D:

| Family | Required body semantics beyond root/center/head/face |
|---|---|
| Kemonomimi | complete humanoid torso, both arms/hands, both legs/feet, both ear bases/tips, tail base/mid/tip |
| Anthro biped | humanoid-compatible torso/limbs, muzzle, jaw, both ear bases/tips, digitigrade toes, tail base/mid/tip |
| Mascot | chest, both arms/hands, both legs/feet, jaw or mouth deformer; non-humanoid 3D uses `GltfRig` |
| Quadruped | pelvis, lower/upper spine, chest, neck, muzzle/jaw, four upper/lower legs, four paws, both ear bases, tail base/mid/tip |
| Avian | chest, neck, beak upper/lower, both wing base/mid/tip chains, both upper/lower legs and feet, tail fan |

All families require blink left/right, gaze axes, at least five viseme
expressions, smile/frown/surprise equivalents, and a head target. Quadrupeds
require front-paw targets and ground; avians require wing targets and ground.
Kemonomimi and anthro bipeds require hand targets. Ear chains are required for
kemonomimi, anthro, and quadruped; tail motion is required wherever the family
definition contains a tail.

The registry loads only the ten expected signed application resources, rejects
duplicate `(family, representation, version)` keys, validates the exact family
requirements, and exposes immutable references after startup.

- [ ] **Step 4: Run registry and completeness tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R "RigDefinitionRegistryTest|RigDefinitionCodecTest" --output-on-failure
```

Expected: all five animal families resolve in 2D and 3D; any missing limb,
facial control, target, ear/tail/wing chain, or format mismatch fails startup.

- [ ] **Step 5: Commit all family definitions**

```powershell
git add src/avatar/RigDefinitionRegistry.h src/avatar/RigDefinitionRegistry.cpp rigs/v1/kemonomimi-2d.json rigs/v1/kemonomimi-3d.json rigs/v1/anthro-biped-2d.json rigs/v1/anthro-biped-3d.json rigs/v1/mascot-2d.json rigs/v1/mascot-3d.json rigs/v1/quadruped-2d.json rigs/v1/quadruped-3d.json rigs/v1/avian-2d.json rigs/v1/avian-3d.json tests/avatar/RigDefinitionRegistryTest.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): register complete animal rig families"
```

## Task 3: Bind and validate the five Inochi2D animal master rigs

**Files:**
- Create: `src/avatar_2d_adapter/Inochi2dRigBinding.h`
- Create: `src/avatar_2d_adapter/Inochi2dRigBinding.cpp`
- Create: `src/avatar_2d_adapter/Inochi2dAnimalRigValidator.h`
- Create: `src/avatar_2d_adapter/Inochi2dAnimalRigValidator.cpp`
- Create: `tests/avatar_2d_adapter/Inochi2dAnimalRigValidatorTest.cpp`
- Modify: `src/avatar_2d_adapter/Inochi2dModuleManifest.h`
- Modify: `src/avatar_2d_adapter/Inochi2dModuleManifest.cpp`
- Modify: `src/avatar_2d_adapter/Inochi2dModuleComposer.cpp`
- Modify: `src/avatar_2d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar2d {

struct Inochi2dRigBinding {
    std::string definitionId;
    std::string definitionVersion;
    std::map<avatar::RigSemantic, std::string> nodeOrDeformerBySemantic;
    std::map<avatar::RigExpressionSemantic, std::string> parameterByExpression;
    std::map<avatar::RigTargetSemantic, std::string> parameterByTarget;
};

class Inochi2dAnimalRigValidator final {
public:
    core::Result<Inochi2dRigBinding> validate(
        const avatar::RigDefinition& definition,
        const InpDocument& document,
        const Inochi2dModuleManifest& manifest) const;
};

} // namespace creator::avatar2d
```

- [ ] **Step 1: Add failing 2D animal-rig tests**

```cpp
TEST(Inochi2dAnimalRigValidatorTest, BindsRealQuadrupedFacePawsAndTail) {
    const auto result = validator().validate(
        definition("quadruped-2d"), quadrupedInp(), quadrupedManifest());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().parameterByExpression.at(
                  RigExpressionSemantic::MuzzleRaise),
              "quad.face.muzzle_raise");
    EXPECT_EQ(result.value().parameterByTarget.at(
                  RigTargetSemantic::FrontPawLeft),
              "quad.body.front_paw_l");
}

TEST(Inochi2dAnimalRigValidatorTest, RejectsDecorativeAnimalPartsWithoutRigging) {
    const auto result = validator().validate(
        definition("avian-2d"), humanInpWithWingPngs(), avianManifest());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove 2D animal validation fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R Inochi2dAnimalRigValidatorTest --output-on-failure
```

Expected: binding and validator types are absent.

- [ ] **Step 3: Implement semantic binding against actual INP contents**

Extend `Inochi2dModuleManifest` with `rigDefinitionId`,
`rigDefinitionVersion`, semantic node/deformer bindings, expression parameter
bindings, target parameter bindings, and secondary-motion group bindings. The
validator resolves every declared runtime name inside the parsed INP, confirms
its object kind, ensures parameter ranges cover the definition, checks that
left/right controls are independent, and verifies every ear, tail, wing, beak,
muzzle, and paw control deforms a non-empty mesh set.

The composer namespaces bindings with the module IDs, preserves them in compiled
metadata, and rejects modules authored for another family or definition major
version. A flat texture layer cannot satisfy a required deformer, target, or
physics chain.

- [ ] **Step 4: Run 2D family binding and deformation tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_2d_tests
ctest --test-dir build/windows-debug -R "Inochi2dAnimalRigValidatorTest|Inochi2dModuleComposerTest" --output-on-failure
```

Expected: each family binds real semantic controls; removing or aliasing a
required deformer, parameter, independent limb, or physics group fails.

- [ ] **Step 5: Commit real Inochi2D animal bindings**

```powershell
git add src/avatar_2d_adapter/Inochi2dRigBinding.h src/avatar_2d_adapter/Inochi2dRigBinding.cpp src/avatar_2d_adapter/Inochi2dAnimalRigValidator.h src/avatar_2d_adapter/Inochi2dAnimalRigValidator.cpp tests/avatar_2d_adapter/Inochi2dAnimalRigValidatorTest.cpp src/avatar_2d_adapter/Inochi2dModuleManifest.h src/avatar_2d_adapter/Inochi2dModuleManifest.cpp src/avatar_2d_adapter/Inochi2dModuleComposer.cpp src/avatar_2d_adapter/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): bind real Inochi2D animal rigs"
```

## Task 4: Bind and validate VRM/glTF animal skeletons

**Files:**
- Create: `src/avatar_3d_adapter/Avatar3dRigBinding.h`
- Create: `src/avatar_3d_adapter/Avatar3dRigBinding.cpp`
- Create: `src/avatar_3d_adapter/Avatar3dAnimalRigValidator.h`
- Create: `src/avatar_3d_adapter/Avatar3dAnimalRigValidator.cpp`
- Create: `tests/avatar_3d_adapter/Avatar3dAnimalRigValidatorTest.cpp`
- Modify: `src/avatar_3d_adapter/Vrm1Document.cpp`
- Modify: `src/avatar_3d_adapter/CreatorStudioRigDocument.h`
- Modify: `src/avatar_3d_adapter/CreatorStudioRigDocument.cpp`
- Modify: `src/avatar_3d_adapter/Avatar3dModuleComposer.cpp`
- Modify: `src/avatar_3d_adapter/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar3d {

struct Avatar3dRigBinding {
    std::string definitionId;
    std::string definitionVersion;
    std::map<avatar::RigSemantic, std::uint32_t> nodeBySemantic;
    std::map<avatar::RigExpressionSemantic, std::string> morphByExpression;
    std::map<avatar::RigTargetSemantic, std::uint32_t> nodeByTarget;
};

class Avatar3dAnimalRigValidator final {
public:
    core::Result<Avatar3dRigBinding> validateVrm(
        const avatar::RigDefinition& definition,
        const Vrm1Document& document) const;
    core::Result<Avatar3dRigBinding> validateGltfRig(
        const avatar::RigDefinition& definition,
        const GlbDocument& glb,
        const CreatorStudioRigDocument& rig) const;
};

} // namespace creator::avatar3d
```

- [ ] **Step 1: Add failing 3D family-format tests**

```cpp
TEST(Avatar3dAnimalRigValidatorTest, AcceptsHumanoidCompatibleAnthroVrm) {
    const auto result = validator().validateVrm(
        definition("anthro-biped-3d"), anthroVrm());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(result.value().nodeBySemantic.contains(RigSemantic::Muzzle));
    EXPECT_TRUE(result.value().nodeBySemantic.contains(RigSemantic::TailTip));
}

TEST(Avatar3dAnimalRigValidatorTest, RequiresTrueQuadrupedLimbTopology) {
    const auto result = validator().validateGltfRig(
        definition("quadruped-3d"), humanGlbWithAnimalMesh(),
        fakeQuadrupedMetadata());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(Avatar3dAnimalRigValidatorTest, NeverAcceptsAvianAsVrm) {
    EXPECT_EQ(validator().validateVrm(definition("avian-3d"), avianShapedVrm())
                  .error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove 3D animal validation fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R Avatar3dAnimalRigValidatorTest --output-on-failure
```

Expected: the family validator and binding are absent.

- [ ] **Step 3: Implement topology and deformation validation**

For VRM, the validator requires the complete VRM humanoid bone map first, then
validates the extra kemonomimi or anthro semantics against unique extension
nodes and expression morphs. It checks parent relationships and non-zero bone
lengths for ear/tail/muzzle/toe chains.

For Creator Studio glTF, the validator requires the exact family definition,
maps every semantic to an in-range node, validates parent topology, detects
cycles, confirms bind matrices and skinned primitive influence, and rejects
semantics that point to empty transforms. Quadruped fore/hind limbs must be four
separate chains; avian wings must be two separate three-segment chains; mascot
limbs must not alias. Expression morphs must affect the declared face
primitives, and every physics chain must contain unique nodes in parent order.

The resulting binding is embedded in `Compiled3dScene` and is the only node map
used by the renderer and later retargeter.

- [ ] **Step 4: Run topology, skinning, and representation tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R "Avatar3dAnimalRigValidatorTest|Vrm1DocumentTest|CreatorStudioRigDocumentTest" --output-on-failure
```

Expected: real family skeletons pass; relabeled human rigs, aliased legs/wings,
empty nodes, invalid parents, and incorrect formats fail.

- [ ] **Step 5: Commit truthful 3D animal bindings**

```powershell
git add src/avatar_3d_adapter/Avatar3dRigBinding.h src/avatar_3d_adapter/Avatar3dRigBinding.cpp src/avatar_3d_adapter/Avatar3dAnimalRigValidator.h src/avatar_3d_adapter/Avatar3dAnimalRigValidator.cpp tests/avatar_3d_adapter/Avatar3dAnimalRigValidatorTest.cpp src/avatar_3d_adapter/Vrm1Document.cpp src/avatar_3d_adapter/CreatorStudioRigDocument.h src/avatar_3d_adapter/CreatorStudioRigDocument.cpp src/avatar_3d_adapter/Avatar3dModuleComposer.cpp src/avatar_3d_adapter/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): validate animal skeleton topology"
```

## Task 5: Author family-specific retarget profiles

**Files:**
- Create: `src/avatar/RigRetargetProfile.h`
- Create: `src/avatar/RigRetargetProfile.cpp`
- Create: `src/avatar/RigRetargetProfileCodec.h`
- Create: `src/avatar/RigRetargetProfileCodec.cpp`
- Create: `schemas/rig-retarget-profile.schema.json`
- Create: `rigs/v1/retarget-kemonomimi.json`
- Create: `rigs/v1/retarget-anthro-biped.json`
- Create: `rigs/v1/retarget-mascot.json`
- Create: `rigs/v1/retarget-quadruped.json`
- Create: `rigs/v1/retarget-avian.json`
- Create: `tests/avatar/RigRetargetProfileTest.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

enum class MotionInputSemantic {
    HeadYaw, HeadPitch, HeadRoll, GazeX, GazeY,
    BlinkLeft, BlinkRight, JawOpen, MouthWide, MouthPucker, Smile, Frown,
    BrowUpLeft, BrowUpRight, BrowDownLeft, BrowDownRight,
    ChestYaw, ChestPitch, ChestRoll, PelvisYaw, PelvisPitch, PelvisRoll,
    HandLeftX, HandLeftY, HandLeftZ, HandRightX, HandRightY, HandRightZ,
    HandLeftOpen, HandRightOpen, VoiceEnergy, IdlePhase
};

enum class RetargetOperator {
    Linear, SmoothStep, DeadZone, Clamp, Invert, Max, Add, Multiply,
    Mirror, PhaseOscillator
};

struct RetargetRule {
    MotionInputSemantic input;
    std::variant<RigSemantic, RigExpressionSemantic, RigTargetSemantic> output;
    std::vector<RetargetOperator> operators;
    float scale;
    float bias;
    float minimum;
    float maximum;
};

struct RigRetargetProfile {
    std::uint32_t schemaVersion;
    RigFamily family;
    std::string rigDefinitionVersion;
    std::vector<RetargetRule> rules;
};

class RigRetargetProfileCodec final {
public:
    static core::Result<RigRetargetProfile> decode(const nlohmann::json& json);
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing authored-behavior tests**

```cpp
TEST(RigRetargetProfileTest, MapsHumanSignalsToAnimalSpecificControls) {
    const auto quad = loadProfile(RigFamily::Quadruped);
    EXPECT_TRUE(hasRule(quad, MotionInputSemantic::JawOpen,
                        RigExpressionSemantic::MuzzleRaise));
    EXPECT_TRUE(hasRule(quad, MotionInputSemantic::HandLeftY,
                        RigTargetSemantic::FrontPawLeft));

    const auto avian = loadProfile(RigFamily::Avian);
    EXPECT_TRUE(hasRule(avian, MotionInputSemantic::JawOpen,
                        RigExpressionSemantic::BeakOpen));
    EXPECT_TRUE(hasRule(avian, MotionInputSemantic::HandRightOpen,
                        RigTargetSemantic::WingRight));
}

TEST(RigRetargetProfileTest, RejectsUnboundedOrMissingRequiredMappings) {
    EXPECT_EQ(decode(profileWithoutBlinkFallback()).error().code(),
              core::ErrorCode::ParseFailure);
    EXPECT_EQ(decode(profileWithNaNScale()).error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove retarget profiles are absent**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R RigRetargetProfileTest --output-on-failure
```

Expected: profile types and five JSON profiles do not exist.

- [ ] **Step 3: Implement validated, family-authored mappings**

The five profiles include:

- kemonomimi: humanoid face/body/hand mappings plus head-motion ear follow,
  smile ear lift, voice-energy ear twitch, and damped tail sway;
- anthro biped: jaw and lip inputs blended into muzzle/visemes, digitigrade
  foot targets, ear emotion, and stronger tail balance;
- mascot: clamped exaggerated head/body motion, two-stage blink, broad mouth
  shapes, short-limb hand/foot targets, and bounded idle squash/stretch;
- quadruped: head/chest/pelvis motion distributed along spine, hands mapped to
  front-paw targets, body lean mapped to hind-leg balance, jaw to muzzle/viseme,
  ear alert, and tail emotion;
- avian: hands mapped to wing targets and fold/spread, jaw to beak open,
  mouth-wide to beak expression, chest motion to wing base, gaze/head mapping,
  and voice-energy feather/tail-fan motion.

Every output has a finite explicit range. Required expressions and targets have
either a direct rule or a named neutral fallback. The codec rejects unsupported
operators, duplicate output writers without an explicit blend operator,
non-finite constants, cycles, and references missing from the family definition.

- [ ] **Step 4: Run profile completeness and canonicalization tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R RigRetargetProfileTest --output-on-failure
```

Expected: all five profiles cover required expressions and targets with bounded
values; human-only blind copies and unresolved outputs fail.

- [ ] **Step 5: Commit family-specific retarget intent**

```powershell
git add src/avatar/RigRetargetProfile.h src/avatar/RigRetargetProfile.cpp src/avatar/RigRetargetProfileCodec.h src/avatar/RigRetargetProfileCodec.cpp schemas/rig-retarget-profile.schema.json rigs/v1/retarget-kemonomimi.json rigs/v1/retarget-anthro-biped.json rigs/v1/retarget-mascot.json rigs/v1/retarget-quadruped.json rigs/v1/retarget-avian.json tests/avatar/RigRetargetProfileTest.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): author animal retarget profiles"
```

## Task 6: Validate part fit, occlusion, and family compatibility

**Files:**
- Create: `src/avatar/RigCompatibilityValidator.h`
- Create: `src/avatar/RigCompatibilityValidator.cpp`
- Create: `tests/avatar/RigCompatibilityValidatorTest.cpp`
- Modify: `src/avatar/AvatarAssetManifest.h`
- Modify: `src/avatar/AvatarAssetManifest.cpp`
- Modify: `src/avatar_2d_adapter/Inochi2dModuleComposer.cpp`
- Modify: `src/avatar_3d_adapter/Avatar3dModuleComposer.cpp`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar {

struct MorphRange {
    std::string name;
    float minimum;
    float maximum;
};

struct PartFitDeclaration {
    std::string masterRigId;
    std::string minimumRigVersion;
    std::string maximumRigVersionExclusive;
    std::vector<RigFamily> families;
    std::vector<MorphRange> supportedMorphs;
    std::vector<AvatarSlot> conflicts;
    std::vector<AvatarSlot> requiresSlots;
    std::vector<std::string> occludesRegions;
};

struct RigCompatibilityIssue {
    enum class Severity { Warning, Error };
    Severity severity;
    AvatarAssetId assetId;
    std::string code;
    std::string messageKey;
};

class RigCompatibilityValidator final {
public:
    std::vector<RigCompatibilityIssue> evaluate(
        const AvatarSpec& spec,
        std::span<const ResolvedAvatarAsset> assets,
        const RigDefinition& rig) const;
};

} // namespace creator::avatar
```

- [ ] **Step 1: Add failing combination tests**

```cpp
TEST(RigCompatibilityValidatorTest, ExplainsCrossFamilyAndMorphRangeFailures) {
    const auto issues = validator().evaluate(
        quadrupedSpecWithHumanoidJacket(), selectedAssets(), quadrupedRig());
    EXPECT_TRUE(hasError(issues, "part.family-incompatible"));

    const auto largeMuzzle = anthroSpec().withMorph("muzzle-length", 1.4F);
    EXPECT_TRUE(hasError(validator().evaluate(
        largeMuzzle, selectedAssets(), anthroRig()), "part.morph-out-of-range"));
}

TEST(RigCompatibilityValidatorTest, AllowsDeclaredOcclusionWithoutDeletingAssets) {
    const auto issues = validator().evaluate(
        avianSpecWithCape(), selectedAssets(), avianRig());
    EXPECT_FALSE(hasError(issues));
    EXPECT_TRUE(compiledSceneRetainsSourceAssetMetadata());
    EXPECT_TRUE(compiledSceneHidesOnlyDeclaredRegion("back-feathers-upper"));
}
```

- [ ] **Step 2: Run and prove compatibility tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests
ctest --test-dir build/windows-debug -R RigCompatibilityValidatorTest --output-on-failure
```

Expected: fit declarations and validator are absent.

- [ ] **Step 3: Implement measurable compatibility rules**

Add `PartFitDeclaration` to every selectable catalog entry. The validator checks
rig/version, family, slot conflicts, required companion slots, morph ranges,
bone/deformer binding, and declared occlusion regions. Errors block compilation.
Warnings are limited to combinations whose authored collision samples exceed a
documented penetration threshold only at the edge of a permitted morph range.

The 2D compiler turns declared regions into exact mask/layer changes. The 3D
compiler disables exact named base primitives. Neither compiler erases an asset
from the spec, silently rescales it beyond its declared range, nor substitutes a
different item. Error `messageKey` values have Korean and English strings in the
later workspace plan.

- [ ] **Step 4: Run cross-family, boundary, and compiler integration tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_tests cs_avatar_2d_tests cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R "RigCompatibilityValidatorTest|Inochi2dModuleComposerTest|Avatar3dModuleComposerTest" --output-on-failure
```

Expected: supported boundary values compile; unsupported families, versions,
morph values, conflicts, and undeclared coverage fail without source mutation.

- [ ] **Step 5: Commit exact animal-part compatibility**

```powershell
git add src/avatar/RigCompatibilityValidator.h src/avatar/RigCompatibilityValidator.cpp tests/avatar/RigCompatibilityValidatorTest.cpp src/avatar/AvatarAssetManifest.h src/avatar/AvatarAssetManifest.cpp src/avatar_2d_adapter/Inochi2dModuleComposer.cpp src/avatar_3d_adapter/Avatar3dModuleComposer.cpp src/avatar/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): enforce animal part fit contracts"
```

## Task 7: Simulate deterministic ears, tails, wings, and mascot motion

**Files:**
- Create: `src/avatar_motion/CMakeLists.txt`
- Create: `src/avatar_motion/AnimalSecondaryMotion.h`
- Create: `src/avatar_motion/AnimalSecondaryMotion.cpp`
- Create: `tests/avatar_motion/AnimalSecondaryMotionTest.cpp`
- Create: `tests/avatar_motion/AnimalRigPhysicsBindingTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/avatar/CMakeLists.txt`
- Modify: `src/avatar_2d_adapter/Inochi2dAvatarCompiler.cpp`
- Modify: `src/avatar_3d_adapter/Avatar3dAvatarCompiler.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar_motion {

struct SecondaryMotionInput {
    core::TimestampNs timestamp;
    std::array<float, 3> rootLinearAcceleration;
    std::array<float, 3> headAngularVelocity;
    float voiceEnergy;
    float emotion;
};

struct SecondaryMotionValue {
    avatar::RigSemantic semantic;
    std::array<float, 4> rotation;
};

class AnimalSecondaryMotion final {
public:
    explicit AnimalSecondaryMotion(
        std::span<const avatar::SecondaryMotionChainDefinition> chains);
    std::vector<SecondaryMotionValue> step(const SecondaryMotionInput& input);
    void reset();
};

} // namespace creator::avatar_motion
```

- [ ] **Step 1: Add failing fixed-step and bound tests**

```cpp
TEST(AnimalSecondaryMotionTest, IsDeterministicAcrossRenderCadences) {
    const auto atThirty = simulate(fixtureChains(), replay(), 30);
    const auto atSixty = simulate(fixtureChains(), replay(), 60);
    EXPECT_NEAR(maxAngularDifference(atThirty, atSixty), 0.0F, 1e-4F);
}

TEST(AnimalSecondaryMotionTest, NeverExceedsAuthoredAnglesAfterLongGap) {
    auto motion = createTailMotion();
    motion.step(inputAt(core::TimestampNs{core::DurationNs{0}}));
    const auto values = motion.step(
        inputAt(core::TimestampNs{core::DurationNs{5'000'000'000}})
            .withAcceleration(100.0F));
    EXPECT_TRUE(allWithinAuthoredBounds(values));
    EXPECT_TRUE(allFinite(values));
}
```

- [ ] **Step 2: Run and prove secondary-motion tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_motion_tests
ctest --test-dir build/windows-debug -R AnimalSecondaryMotionTest --output-on-failure
```

Expected: the simulator does not exist.

- [ ] **Step 3: Implement bounded fixed-step simulation**

Use a 120 Hz fixed integration step, a maximum eight substeps per rendered
frame, semi-implicit damped spring integration, normalized quaternions, and
per-chain authored angular limits. Gaps above 250 ms reset velocity and blend
from the last valid pose over 150 ms. Ear alert and tail/feather emotion are
bounded target offsets, not accumulated impulses. A seeded phase oscillator
drives idle-only motion; deterministic replay stores the seed.

The 2D compiler binds output rotations to normalized physics parameters. The 3D
compiler binds them to semantic chain nodes. When a runtime has native physics,
the same coefficients and fixed timestamps are forwarded and the native path is
checked against the reference simulator's tolerance.

- [ ] **Step 4: Run deterministic, stress, and adapter tests**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_motion_tests cs_avatar_2d_tests cs_avatar_3d_adapter_tests
ctest --test-dir build/windows-debug -R "AnimalSecondaryMotionTest|AnimalRigPhysicsBindingTest" --output-on-failure
```

Expected: 30/60/120 Hz replays converge, long gaps and extreme inputs remain
finite and bounded, and both representation adapters move the declared chains.

- [ ] **Step 5: Commit production secondary motion**

```powershell
git add src/avatar_motion/CMakeLists.txt src/avatar_motion/AnimalSecondaryMotion.h src/avatar_motion/AnimalSecondaryMotion.cpp tests/avatar_motion/AnimalSecondaryMotionTest.cpp tests/avatar_motion/AnimalRigPhysicsBindingTest.cpp CMakeLists.txt src/avatar/CMakeLists.txt src/avatar_2d_adapter/Inochi2dAvatarCompiler.cpp src/avatar_3d_adapter/Avatar3dAvatarCompiler.cpp tests/CMakeLists.txt
git commit -m "feat(avatar): simulate bounded animal secondary motion"
```

## Task 8: Qualify every animal family in both representations

**Files:**
- Create: `tests/fixtures/avatar-animals/README.md`
- Create: `tests/acceptance/AnimalRigFamiliesAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `legal/ASSET_BOM.csv`
- Modify: `README.md`

**Interfaces:**
- Consumes: ten rig definitions, five retarget profiles, both compilers and
  renderers, compatibility validation, and secondary motion.
- Produces: no-decorative-substitute animal release gate.

- [ ] **Step 1: Add the failing representation/family matrix test**

```cpp
class AnimalRigFamiliesAcceptanceTest :
    public testing::TestWithParam<std::tuple<RigFamily, AvatarRepresentation>> {};

TEST_P(AnimalRigFamiliesAcceptanceTest, CompilesAndMovesFamilySpecificControls) {
    const auto [family, representation] = GetParam();
    const auto compiled = compilerFor(representation).compile(
        requestFor(fixtureSpec(family, representation)));
    ASSERT_TRUE(compiled.hasValue()) << compiled.error().message();
    EXPECT_TRUE(hasAllRequiredSemanticBindings(compiled.value(), family));
    EXPECT_TRUE(faceInputChangesPixels(compiled.value(), family));
    EXPECT_TRUE(bodyInputChangesPixels(compiled.value(), family));
    EXPECT_TRUE(familySpecificInputChangesPixels(compiled.value(), family));
    EXPECT_TRUE(secondaryMotionChangesPixels(compiled.value(), family));
}

INSTANTIATE_TEST_SUITE_P(
    AllAnimalRigs, AnimalRigFamiliesAcceptanceTest,
    testing::Values(
        std::tuple{RigFamily::Kemonomimi, AvatarRepresentation::Inochi2d},
        std::tuple{RigFamily::Kemonomimi, AvatarRepresentation::Vrm1},
        std::tuple{RigFamily::AnthroBiped, AvatarRepresentation::Inochi2d},
        std::tuple{RigFamily::AnthroBiped, AvatarRepresentation::Vrm1},
        std::tuple{RigFamily::Mascot, AvatarRepresentation::Inochi2d},
        std::tuple{RigFamily::Mascot, AvatarRepresentation::GltfRig},
        std::tuple{RigFamily::Quadruped, AvatarRepresentation::Inochi2d},
        std::tuple{RigFamily::Quadruped, AvatarRepresentation::GltfRig},
        std::tuple{RigFamily::Avian, AvatarRepresentation::Inochi2d},
        std::tuple{RigFamily::Avian, AvatarRepresentation::GltfRig}));
```

- [ ] **Step 2: Run and prove the ten-cell matrix fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_animal_rig_families_acceptance_tests
ctest --test-dir build/windows-debug -R AnimalRigFamiliesAcceptanceTest --output-on-failure
```

Expected: the complete signed fixture matrix and acceptance target are absent.

- [ ] **Step 3: Add final-quality signed family fixtures**

Add one production-quality acceptance avatar for every matrix cell. Each 2D
fixture has layered source art, real deformers, family face controls, independent
limb or wing/paw controls, masks, color channels, and ear/tail/wing/clothing
physics where applicable. Each 3D fixture has a skinned mesh, family topology,
face morphs, MToon materials, four LODs, and family secondary-motion chains.

The fixtures must visibly distinguish:

- kemonomimi ear emotion and tail motion;
- anthro muzzle visemes, digitigrade feet, and tail balance;
- mascot squash/stretch and short-limb targets;
- quadruped four-paw/spine motion and muzzle expression;
- avian independent wing fold/spread, beak expression, and tail fan.

Every fixture is referenced by a signed package manifest and a complete
`legal/ASSET_BOM.csv` row. Static images, rigid meshes, renamed human rigs, or
shared aliases for independent limbs cannot pass the semantic and pixel-change
assertions.

- [ ] **Step 4: Run the full animal qualification gate**

Run:

```powershell
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug -R "AnimalRigFamiliesAcceptanceTest|RigDefinition.*Test|AnimalRig.*Test|RigCompatibilityValidatorTest" --output-on-failure
ctest --test-dir build/windows-debug --output-on-failure
git diff --check
```

Expected: all ten family/representation cells compile, render, react to
family-specific motion, and retain valid transparency and rights evidence; the
full suite passes.

- [ ] **Step 5: Commit animal-family qualification**

```powershell
git add tests/fixtures/avatar-animals/README.md tests/acceptance/AnimalRigFamiliesAcceptanceTest.cpp tests/CMakeLists.txt legal/ASSET_BOM.csv README.md
git commit -m "test(avatar): qualify animal rigs in 2d and 3d"
```

## Plan Completion Gate

- Ten immutable rig definitions cover five animal families in 2D and 3D.
- Inochi2D bindings point to real deformers and parameters; 3D bindings point to
  valid VRM or Creator Studio skeleton nodes and morphs.
- Authored profiles map canonical motion to muzzle, beak, paw, wing, ear, tail,
  and mascot behavior instead of copying human values blindly.
- Part fit, morph ranges, conflicts, and occlusion are explicit and enforced by
  both compilers.
- Secondary motion is deterministic, bounded, and connected to actual runtime
  controls.
- Final-quality signed fixtures prove every family/representation cell through
  family-specific pixel changes; decorative, static, rigid, or relabeled
  substitutes fail.
