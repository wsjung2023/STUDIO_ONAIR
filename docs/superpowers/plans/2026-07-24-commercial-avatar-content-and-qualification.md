# Commercial Avatar Content and Qualification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Content-production steps require the named art, rigging, legal, and QA sign-offs in addition to automated tests. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a commercially cleared, visually coherent, genuinely diverse 2D/3D avatar catalog at the approved minimum scale, and block release unless real assets pass rights, rigging, customization, rendering, cross-platform, performance, stability, accessibility, and end-to-end output qualification.

**Architecture:** Versioned content manifests and an art-direction contract make every commissioned asset measurable. A Qt-free content auditor loads source manifests, signed packs, catalog entries, rights records, both compilers, and both renderers to prove counts and behavior rather than trusting filenames or thumbnails. Deterministic render/contact sheets support human art and clipping review. A signed release catalog is produced only from approved immutable asset hashes, then physical-device harnesses exercise the exact release build for two hours. One machine-readable launch verdict aggregates every gate and refuses partial or self-attested success.

**Tech Stack:** C++20, existing avatar package/catalog/license/2D/3D/tracking/output modules, nlohmann/json 3.11.3, CMake/CTest, Qt 6.8 Test, PowerShell 7, Android Debug Bridge for physical Android qualification, Git LFS for authored binary assets, GoogleTest 1.15.2.

## Global Constraints

- Requires completion of the other six implementation plans.
- Authoritative design: `docs/superpowers/specs/2026-07-24-production-avatar-creator-design.md`.
- This is the release gate, not a sample-content gate. Test models, cubes,
  triangles, flat cards, static portraits, recolors, renamed duplicates, public
  demo rigs, and watermarked previews do not count toward catalog minimums.
- Minimum counts apply independently to 2D and 3D:
  - at least three final editable presets for each of six rig families;
  - all eight named themes;
  - at least 30 unique layered/composable hair designs;
  - at least 24 unique mix-and-match outfit sets;
  - at least 50 unique accessories covering every required accessory category.
- A palette, texture, or minor ornament variant of one `designRootId` is one
  design for minimum-count purposes. Variants remain useful catalog items but
  cannot inflate coverage.
- Every bundled selectable asset must permit commercial broadcast/recording by
  individuals and corporations, Creator Studio application bundling, and
  customization/derivative character creation. Model-file export is a separate
  per-asset right and may be denied.
- Rights approval requires traceable contract or license evidence, exact file
  hashes, creator/source, attribution, territory, term, restrictions, reviewer,
  and review date. “Royalty free”, a marketplace URL, or a code-library license
  without model-specific evidence is insufficient.
- Sensitive contracts are not committed. `legal/ASSET_BOM.csv` stores an opaque
  evidence ID and SHA-256 of the approved evidence held in the legal evidence
  store; CI receives only approval metadata and hashes.
- Asset creators warrant originality or fully document third-party inputs,
  trademarks, fonts, brushes, textures, scans, motion, and generative tools.
  Undocumented generative or third-party material is release-blocking.
- 2D and 3D do not need pixel-identical assets, but every catalog item has
  semantic theme/category tags so users can find equivalent style intent.
- Each preset and part must customize and deform through actual compiler/runtime
  controls. A polished image or mesh with disconnected controls fails.
- Thumbnails and store images are generated from the same signed release asset,
  compiler, and renderer. Marketing art cannot hide a lower-quality runtime
  item.
- Automated visual metrics assist but do not replace art-director and rigging
  sign-off. Human sign-off cannot waive schema, rights, format, security,
  performance, or runtime failures.
- A qualification run is valid only on a physical device whose recorded
  identity matches the named tier. Virtual machines, software GPU, simulators,
  emulators, and unmatched hardware are reported as `not-qualified`.
- Required physical tiers:
  - Windows with an RTX 3060-class GPU at 1080p60;
  - macOS with Apple M1 at 1080p60;
  - Android with Snapdragon 8 Gen 1 at 720p30;
  - one higher Android tier at 1080p30.
- Motion-input-to-preview p95 must be at most 80 ms on Windows/M1 and 120 ms on
  both Android tiers. Two-hour tracking/render/recording is mandatory for 2D
  human, 2D animal, 3D human, and 3D animal scenarios on every tier.
- Release cannot downgrade an unmet tier, omit an animal family, hide a rights
  denial, or mark `not-qualified` as pass.
- All generated qualification artifacts include source commit, clean/dirty
  state, build preset, compiler, dependencies, OS, device, driver, asset catalog
  hash, configuration, start/end UTC, and tool version.

---

## Approved Content Matrix

### Presets per representation

| Rig family | Three required distinct directions |
|---|---|
| Humanoid | modern creator, formal/performer, fantasy or cyber adventurer |
| Kemonomimi | feline, fox/canine, rabbit or horned hybrid |
| AnthroBiped | feline, canine, rabbit/reptile with real muzzle/feet silhouettes |
| Mascot | round soft mascot, angular creature mascot, compact themed performer |
| Quadruped | cat, dog, fox/rabbit with true four-leg topology |
| Avian | songbird, owl/raptor, corvid or fantasy bird with true wing topology |

Each direction must differ in silhouette, face design, color/pattern strategy,
outfit or body styling, and family behavior. The same base with new colors is
not a distinct preset.

### Themes per representation

`modern-casual`, `fantasy`, `cyber`, `formal`, `idol`,
`street`, `nature`, and `mascot`.

Every theme has at least one complete compatible demonstration look and
contains hair/head styling when applicable, outfit/body styling, accessory,
palette, and saved preset. Animal body styling may replace conventional hair or
clothing only when the family anatomy makes that semantically correct.

### Unique part minimums per representation

| Category | Minimum | Required internal coverage |
|---|---:|---|
| Hair/head styling | 30 | short/medium/long, straight/wave/curl, tied, layered front/side/back, textured/fur/feather alternatives |
| Outfit sets | 24 | each of eight themes has three mix-and-match sets; separate compatible inner/top/bottom/outerwear/footwear or family-correct body pieces |
| Accessories | 50 | eyewear, jewelry, hats/headwear, horns, ears, tails, wings all represented; face/body accessories and animal-safe variants |

## Quality Budgets

| Asset path | Desktop high | Mobile |
|---|---|---|
| 2D | max four 4096² atlases, ≤600 draw parts, ≤256 MiB texture residency | max two 2048² atlases, ≤400 draw parts, ≤64 MiB residency |
| 3D | ≤160k visible triangles LOD0, ≤48 draw calls, ≤256 MiB texture residency | ≤65k visible triangles, ≤28 draw calls, ≤96 MiB residency |

All 3D assets include four useful LODs and all 2D assets include high/mobile
atlas variants. Budgets apply to a complete preset plus one outfit and five
accessories, not isolated naked bases.

## File Structure

```text
content/avatar/
  ART_DIRECTION.md
  TECHNICAL_ART_STANDARD.md
  NAMING_AND_TAGGING.md
  CONTENT_MATRIX.csv
  catalog/
    catalog-source.json
    semantic-tag-map.json
    release-catalog.schema.json
  palettes/
    skin-and-marking-palettes.json
  presets/
    2d/*.avatar-spec.json
    3d/*.avatar-spec.json
  packs/
    2d/*/pack-source.json
    3d/*/pack-source.json
  reviews/
    visual-review.schema.json
    rig-review.schema.json
    review-index.json
legal/
  AVATAR_ASSET_CONTRACT_REQUIREMENTS.md
  ASSET_BOM.csv
  ASSET_BOM.schema.json
tools/avatar-content/
  CMakeLists.txt
  AvatarContentAuditMain.cpp
  ContentMatrixAudit.h/.cpp
  AssetRightsAudit.h/.cpp
  Avatar2dContentAudit.h/.cpp
  Avatar3dContentAudit.h/.cpp
  CatalogSemanticAudit.h/.cpp
  VisualContactSheet.h/.cpp
  CombinationQualification.h/.cpp
  ReleaseCatalogBuilder.h/.cpp
scripts/
  build_avatar_release_catalog.ps1
  audit_avatar_content.ps1
  render_avatar_review_sheets.ps1
  run_avatar_device_qualification.ps1
  run_avatar_stability_qualification.ps1
  verify_avatar_release.ps1
tests/scripts/
  AvatarReleaseVerdictTest.ps1
schemas/
  avatar-content-matrix.schema.json
  avatar-qualification-result.schema.json
release/avatar/
  qualification/README.md
tests/avatar_content/
  ContentMatrixAuditTest.cpp
  AssetRightsAuditTest.cpp
  Avatar2dContentAuditTest.cpp
  Avatar3dContentAuditTest.cpp
  CatalogSemanticAuditTest.cpp
  CombinationQualificationTest.cpp
  ReleaseCatalogBuilderTest.cpp
tests/acceptance/
  CommercialAvatarCatalogAcceptanceTest.cpp
  AvatarCrossPlatformAcceptanceTest.cpp
  AvatarLongRunAcceptanceTest.cpp
```

## Task 1: Freeze art direction, technical standards, naming, and count rules

**Files:**
- Create: `content/avatar/ART_DIRECTION.md`
- Create: `content/avatar/TECHNICAL_ART_STANDARD.md`
- Create: `content/avatar/NAMING_AND_TAGGING.md`
- Create: `content/avatar/CONTENT_MATRIX.csv`
- Create: `schemas/avatar-content-matrix.schema.json`
- Create: `tools/avatar-content/CMakeLists.txt`
- Create: `tools/avatar-content/AvatarContentAuditMain.cpp`
- Create: `tools/avatar-content/ContentMatrixAudit.h`
- Create: `tools/avatar-content/ContentMatrixAudit.cpp`
- Create: `tests/avatar_content/ContentMatrixAuditTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar_content {

struct ContentMatrixRow {
    std::string catalogId;
    std::string designRootId;
    avatar::AvatarRepresentation representation;
    avatar::RigFamily family;
    std::string category;
    avatar::AvatarSlot slot;
    std::vector<std::string> themes;
    std::vector<std::string> semanticTags;
    std::string packSourcePath;
    std::string rightsAssetId;
    std::string artReviewId;
    std::string rigReviewId;
};

struct ContentCoverage {
    std::map<avatar::AvatarRepresentation,
             std::map<avatar::RigFamily, std::size_t>> presetDesigns;
    std::map<avatar::AvatarRepresentation, std::size_t> hairDesigns;
    std::map<avatar::AvatarRepresentation, std::size_t> outfitDesigns;
    std::map<avatar::AvatarRepresentation, std::size_t> accessoryDesigns;
    std::map<avatar::AvatarRepresentation,
             std::set<std::string>> themes;
};

class ContentMatrixAudit final {
public:
    core::Result<ContentCoverage> audit(
        const std::filesystem::path& csvPath) const;
};

} // namespace creator::avatar_content
```

- [ ] **Step 1: Add failing coverage and anti-inflation tests**

```cpp
TEST(ContentMatrixAuditTest, RequiresMinimumsIndependentlyFor2dAnd3d) {
    const auto result = audit(matrixWith(
        presetsPerFamily(2), hair(29), outfits(23), accessories(49),
        themes({"modern-casual", "fantasy"})));
    ASSERT_FALSE(result.hasValue());
    EXPECT_THAT(result.error().message(),
                HasSubstr("inochi2d humanoid presets: 2 < 3"));
    EXPECT_THAT(result.error().message(),
                HasSubstr("vrm/gltf hair designs: 29 < 30"));
}

TEST(ContentMatrixAuditTest, CountsDesignRootsNotColorVariants) {
    const auto result = audit(matrixWithThirtyRowsSharingOneDesignRoot());
    ASSERT_FALSE(result.hasValue());
    EXPECT_THAT(result.error().message(),
                HasSubstr("unique hair design roots: 1"));
}

TEST(ContentMatrixAuditTest, RequiresAllFamiliesThemesAndAccessoryKinds) {
    const auto result = audit(matrixMissingAvianAndEyewear());
    ASSERT_FALSE(result.hasValue());
    EXPECT_THAT(result.error().message(), HasSubstr("avian"));
    EXPECT_THAT(result.error().message(), HasSubstr("eyewear"));
}
```

- [ ] **Step 2: Run and prove content governance is absent**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_content_tests
ctest --test-dir build/windows-debug -R ContentMatrixAuditTest --output-on-failure
```

Expected: standards, matrix schema, auditor, and test target are absent.

- [ ] **Step 3: Author enforceable art and technical contracts**

`ART_DIRECTION.md` defines high-end anime 2D and cel-shaded anime 3D:
silhouette readability at 100%, 50%, and 25% broadcast scale; line-weight and
shape language; face feature hierarchy; cel-light/shade/rim/outline rules;
texture frequency; palette contrast; theme mood boards by approved internal
reference IDs; prohibited copied franchise/trademark designs; and separate
human, fur, scale, feather, mascot, and garment treatments.

It requires every preset to have front/three-quarter/profile/back review,
neutral and maximum expressions, family motion poses, transparent/light/dark/
complex backgrounds, and a 256-pixel-tall broadcast view. It defines acceptance
examples using rights-cleared internal fixtures, not third-party screenshots.

`TECHNICAL_ART_STANDARD.md` fixes:

- package/catalog/rig schema versions and ID format;
- 2D layer/mask/deformer/parameter/color/physics requirements, atlas budgets,
  edge padding, transparent RGB cleanup, and authored angle coverage;
- 3D meters/right-handed coordinates, topology, UV/tangent, skin-weight,
  morph, MToon, transparency, LOD, collision, and texture budgets;
- required family semantics from the animal-rig definitions;
- body/morph fit ranges, coverage regions, slot dependencies/conflicts;
- thumbnail camera/light/background and actual-runtime generation;
- source-tool version/export settings recorded per pack.

`NAMING_AND_TAGGING.md` defines stable IDs, `designRootId`, Korean/English names,
the eight exact themes, family/slot/species/style/length/texture/material/color/
rights tags, and semantic 2D↔3D correspondence. Tags describe delivered content
and cannot be added merely to satisfy coverage.

`CONTENT_MATRIX.csv` has one row per catalog item with the fields in
`ContentMatrixRow`. The initial check-in contains headers plus every commissioned
deliverable row and status `commissioned`, with no fake files or approval
values. The auditor validates CSV/schema, unique IDs, valid paths, closed tags,
all count/category/family/theme rules, and counts only rows whose later release
status is `approved`.

- [ ] **Step 4: Run standards and initial commissioned-matrix validation**

Run:

```powershell
cmake --build --preset windows-debug --target avatar-content-audit cs_avatar_content_tests
ctest --test-dir build/windows-debug -R ContentMatrixAuditTest --output-on-failure
build/windows-debug/tools/avatar-content/avatar-content-audit.exe matrix --input content/avatar/CONTENT_MATRIX.csv --allow-status commissioned
```

Expected: schema and anti-inflation tests pass, the commissioned matrix contains
all required deliverables and valid stable IDs, and release coverage still
fails until rows become approved.

- [ ] **Step 5: Commit the production contract**

```powershell
git add content/avatar/ART_DIRECTION.md content/avatar/TECHNICAL_ART_STANDARD.md content/avatar/NAMING_AND_TAGGING.md content/avatar/CONTENT_MATRIX.csv schemas/avatar-content-matrix.schema.json tools/avatar-content/CMakeLists.txt tools/avatar-content/AvatarContentAuditMain.cpp tools/avatar-content/ContentMatrixAudit.h tools/avatar-content/ContentMatrixAudit.cpp tests/avatar_content/ContentMatrixAuditTest.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "docs(avatar): freeze commercial content standards"
```

## Task 2: Make asset-specific commercial rights a hard release prerequisite

**Files:**
- Create: `legal/AVATAR_ASSET_CONTRACT_REQUIREMENTS.md`
- Create: `legal/ASSET_BOM.schema.json`
- Create: `tools/avatar-content/AssetRightsAudit.h`
- Create: `tools/avatar-content/AssetRightsAudit.cpp`
- Create: `tests/avatar_content/AssetRightsAuditTest.cpp`
- Modify: `legal/ASSET_BOM.csv`
- Modify: `tools/avatar-content/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar_content {

struct AssetEvidenceRecord {
    avatar::AvatarAssetId assetId;
    std::string version;
    std::string assetSha256;
    std::string creatorOrLicensor;
    std::string evidenceId;
    std::string evidenceSha256;
    std::string evidenceType;
    avatar::GrantState commercialBroadcastIndividual;
    avatar::GrantState commercialBroadcastCorporation;
    avatar::GrantState appBundle;
    avatar::GrantState derivativeCharacter;
    avatar::GrantState modelExport;
    avatar::GrantState rawPartRedistribution;
    avatar::GrantState portableProject;
    std::string attribution;
    std::string territory;
    std::string validFrom;
    std::string validUntil;
    std::string reviewer;
    std::string reviewedAt;
};

class AssetRightsAudit final {
public:
    core::Result<void> auditBundledCatalog(
        std::span<const ContentMatrixRow> content,
        std::span<const AssetEvidenceRecord> evidence,
        core::Utc releaseAt) const;
};

} // namespace creator::avatar_content
```

- [ ] **Step 1: Add failing incomplete/evasive-rights tests**

```cpp
TEST(AssetRightsAuditTest, RequiresBundlingDerivativeAndBothCommercialUsers) {
    auto row = validEvidence();
    row.appBundle = GrantState::Unknown;
    EXPECT_EQ(audit(row).error().code(), core::ErrorCode::InvalidState);
    row = validEvidence();
    row.commercialBroadcastCorporation = GrantState::Denied;
    EXPECT_EQ(audit(row).error().code(), core::ErrorCode::InvalidState);
}

TEST(AssetRightsAuditTest, RejectsGenericLibraryLicenseForModelFiles) {
    auto row = validEvidence();
    row.evidenceType = "software-library-license";
    EXPECT_EQ(audit(row).error().code(), core::ErrorCode::InvalidArgument);
}

TEST(AssetRightsAuditTest, AllowsExportDeniedWhenAllBundledUsesAreApproved) {
    auto row = validEvidence();
    row.modelExport = GrantState::Denied;
    EXPECT_TRUE(audit(row).hasValue());
}
```

- [ ] **Step 2: Run and prove rights audit fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_content_tests
ctest --test-dir build/windows-debug -R AssetRightsAuditTest --output-on-failure
```

Expected: contract requirements, strict BOM schema, and auditor are absent.

- [ ] **Step 3: Define acquisition clauses and validate evidence**

`AVATAR_ASSET_CONTRACT_REQUIREMENTS.md` requires:

1. named creator/licensor and authority to grant rights;
2. original asset scope tied to deliverable IDs and hashes;
3. worldwide, perpetual or release-term-covered individual/corporate commercial
   broadcast and recording monetization;
4. distribution inside Creator Studio and signed content packs;
5. user color/material/morph/part customization and derivative character use;
6. explicit decisions for completed-model export, raw-part redistribution, and
   portable project packages;
7. sublicense/redistribution mechanics necessary for end-user use;
8. attribution, trademark/personality/privacy, moral-rights, revocation, and
   termination terms;
9. all third-party tools/materials and generative systems disclosed with their
   terms and human contribution;
10. source delivery, correction warranty, confidentiality, and takedown process.

The asset team cannot mark rows approved. Legal supplies an opaque evidence ID,
evidence file SHA-256, exact decisions, and reviewer/date after comparing the
final delivery hashes. The auditor validates the strict CSV schema, unique
asset/version/hash, approved evidence types, evidence hash shape, release date
within term, required bundled permissions all `Allowed`, attribution data,
catalog-to-BOM completeness, and manifest-to-BOM equality.

Export may be `Denied` and is surfaced by the product. Raw-part redistribution
may be denied. No conditional value is accepted for a bundled required use
unless the condition is machine-represented and satisfied by the release
configuration.

- [ ] **Step 4: Run rights completeness against commissioned inventory**

Run:

```powershell
cmake --build --preset windows-debug --target avatar-content-audit cs_avatar_content_tests
ctest --test-dir build/windows-debug -R AssetRightsAuditTest --output-on-failure
build/windows-debug/tools/avatar-content/avatar-content-audit.exe rights --matrix content/avatar/CONTENT_MATRIX.csv --bom legal/ASSET_BOM.csv --release-date 2026-07-24
```

Expected: unit tests pass; commissioned items without final evidence are listed
by exact asset ID and the release audit remains failed until legal approval.

- [ ] **Step 5: Commit the rights gate**

```powershell
git add legal/AVATAR_ASSET_CONTRACT_REQUIREMENTS.md legal/ASSET_BOM.schema.json legal/ASSET_BOM.csv tools/avatar-content/AssetRightsAudit.h tools/avatar-content/AssetRightsAudit.cpp tests/avatar_content/AssetRightsAuditTest.cpp tools/avatar-content/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(avatar): require asset specific commercial evidence"
```

## Task 3: Audit real 2D and 3D source assets, rigs, variants, and budgets

**Files:**
- Create: `tools/avatar-content/Avatar2dContentAudit.h`
- Create: `tools/avatar-content/Avatar2dContentAudit.cpp`
- Create: `tools/avatar-content/Avatar3dContentAudit.h`
- Create: `tools/avatar-content/Avatar3dContentAudit.cpp`
- Create: `tests/avatar_content/Avatar2dContentAuditTest.cpp`
- Create: `tests/avatar_content/Avatar3dContentAuditTest.cpp`
- Modify: `tools/avatar-content/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar_content {

struct AssetTechnicalAuditResult {
    avatar::AvatarAssetId assetId;
    std::string version;
    std::string sourceSha256;
    std::string runtimeSha256;
    bool deforming;
    bool customizable;
    bool budgetCompliant;
    std::vector<std::string> measuredControls;
    std::vector<std::string> issues;
};

class Avatar2dContentAudit final {
public:
    core::Result<AssetTechnicalAuditResult> audit(
        const avatar::ResolvedAvatarAsset& asset,
        const avatar::RigDefinition& rig) const;
};

class Avatar3dContentAudit final {
public:
    core::Result<AssetTechnicalAuditResult> audit(
        const avatar::ResolvedAvatarAsset& asset,
        const avatar::RigDefinition& rig) const;
};

} // namespace creator::avatar_content
```

- [ ] **Step 1: Add failing rigid/static/over-budget tests**

```cpp
TEST(Avatar2dContentAuditTest, RejectsFlatArtAndDisconnectedCustomization) {
    EXPECT_EQ(audit(flatPngModule()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(audit(rigWhoseColorChannelChangesNoPixels()).error().code(),
              core::ErrorCode::InvalidState);
}

TEST(Avatar3dContentAuditTest, RejectsRigidMeshesFakeLodsAndExcessBudget) {
    EXPECT_EQ(audit(meshWithNoWeightedVertices()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(audit(meshWhoseFourLodsHaveSameIndices()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(audit(completeLookWith300MiBMobileTextures()).error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove technical content audit fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_content_tests
ctest --test-dir build/windows-debug -R "Avatar2dContentAuditTest|Avatar3dContentAuditTest" --output-on-failure
```

Expected: representation-specific content auditors are absent.

- [ ] **Step 3: Implement behavior- and budget-based asset inspection**

For every 2D asset, load its signed package and actual INP/module:

- validate declared layers, mesh coverage, masks, deformers, parameters, color
  channels, physics groups, family bindings, slot and fit ranges;
- render neutral plus each declared control at minimum/mid/maximum and require a
  meaningful pixel/geometry change within the declared affected region;
- verify left/right independence, no transparent RGB fringe, atlas padding,
  high/mobile atlas source relation, draw-part and memory budgets;
- reject a runtime texture copied into multiple “source” designs or thumbnails
  not generated from the runtime hash.

For every 3D asset:

- validate topology, normals/tangents/UVs, non-zero normalized skin weights,
  bone influence, morph deltas, expression regions, material semantics, alpha,
  spring chains, family topology, fit/morph ranges;
- verify LOD triangle counts and screen-space silhouette/UV/morph preservation,
  not merely distinct filenames;
- render skin/morph/material/physics controls and require expected pixel/bounds
  changes;
- enforce complete-look triangle/draw-call/texture budgets for high/mobile.

Both auditors compute source/runtime hashes, normalized texture perceptual
hashes, geometry fingerprints, and control-response signatures. Identical or
near-identical `designRootId` candidates are flagged for human duplicate review
and count as one until resolved.

- [ ] **Step 4: Run audits over every delivered pack**

Run:

```powershell
cmake --build --preset windows-debug --target avatar-content-audit cs_avatar_content_tests
ctest --test-dir build/windows-debug -R "Avatar2dContentAuditTest|Avatar3dContentAuditTest" --output-on-failure
build/windows-debug/tools/avatar-content/avatar-content-audit.exe technical --matrix content/avatar/CONTENT_MATRIX.csv --pack-root content/avatar/packs --output build/content-audit/technical.json
```

Expected: every delivery receives exact control/budget measurements; rigid,
static, disconnected, fake-LOD, duplicated, or over-budget assets remain
unapproved with issue codes.

- [ ] **Step 5: Commit technical asset auditing**

```powershell
git add tools/avatar-content/Avatar2dContentAudit.h tools/avatar-content/Avatar2dContentAudit.cpp tools/avatar-content/Avatar3dContentAudit.h tools/avatar-content/Avatar3dContentAudit.cpp tests/avatar_content/Avatar2dContentAuditTest.cpp tests/avatar_content/Avatar3dContentAuditTest.cpp tools/avatar-content/CMakeLists.txt tests/CMakeLists.txt
git commit -m "test(avatar): audit real asset deformation and budgets"
```

## Task 4: Produce and approve the complete 2D/3D launch catalog

**Files:**
- Create: `content/avatar/catalog/catalog-source.json`
- Create: `content/avatar/catalog/semantic-tag-map.json`
- Create: `content/avatar/catalog/release-catalog.schema.json`
- Create: `content/avatar/catalog/approved-source-paths.txt`
- Create: `content/avatar/palettes/skin-and-marking-palettes.json`
- Create: `content/avatar/reviews/visual-review.schema.json`
- Create: `content/avatar/reviews/rig-review.schema.json`
- Create: `content/avatar/reviews/review-index.json`
- Create: `tools/avatar-content/CatalogSemanticAudit.h`
- Create: `tools/avatar-content/CatalogSemanticAudit.cpp`
- Create: `tools/avatar-content/VisualContactSheet.h`
- Create: `tools/avatar-content/VisualContactSheet.cpp`
- Create: `scripts/render_avatar_review_sheets.ps1`
- Create: `tests/avatar_content/CatalogSemanticAuditTest.cpp`
- Modify: `content/avatar/ART_DIRECTION.md`
- Modify: `content/avatar/TECHNICAL_ART_STANDARD.md`
- Modify: `content/avatar/NAMING_AND_TAGGING.md`
- Modify: `content/avatar/CONTENT_MATRIX.csv`
- Modify: `legal/ASSET_BOM.csv`
- Modify: `tools/avatar-content/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Input asset trees: `content/avatar/packs/2d/*` and
  `content/avatar/packs/3d/*`, stored with Git LFS where binary.
- Output review sheets: `build/avatar-review/<representation>/<family>/`.
- Review index references immutable runtime asset and sheet hashes.

- [ ] **Step 1: Add failing semantic diversity and review-completeness tests**

```cpp
TEST(CatalogSemanticAuditTest, RequiresMeaningEquivalentTagsAcrossRepresentations) {
    const auto result = audit(catalogWith2dCyberButNo3dCyber());
    ASSERT_FALSE(result.hasValue());
    EXPECT_THAT(result.error().message(),
                HasSubstr("semantic theme cyber missing in 3d"));
}

TEST(CatalogSemanticAuditTest, RequiresDistinctApprovedReviewsForEveryAsset) {
    const auto result = audit(catalogWhoseRecolorVariantsShareOneReview());
    ASSERT_FALSE(result.hasValue());
    EXPECT_THAT(result.error().message(), HasSubstr("review hash mismatch"));
}

TEST(CatalogSemanticAuditTest, SkinFurScaleAndFeatherPalettesRemainReadable) {
    EXPECT_TRUE(audit(completePaletteFixture()).hasValue());
    EXPECT_EQ(audit(paletteWithLowFaceContrast()).error().code(),
              core::ErrorCode::InvalidState);
}
```

- [ ] **Step 2: Run and prove launch catalog/reviews are absent**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_content_tests
ctest --test-dir build/windows-debug -R CatalogSemanticAuditTest --output-on-failure
```

Expected: catalog source, palette system, review schemas/index, and semantic
auditor are absent.

- [ ] **Step 3: Deliver actual authored packs and generate immutable reviews**

Art/rigging production delivers all approved-matrix source manifests and signed
pack inputs:

- 18 complete 2D presets and 18 complete 3D presets covering the six families;
- 30 unique 2D and 30 unique 3D hair/head-style design roots;
- 24 unique 2D and 24 unique 3D outfit-set design roots, three per theme;
- 50 unique 2D and 50 unique 3D accessory design roots with all required kinds;
- palettes/patterns tested on light/dark skin, fur, scales, feathers, and mascot
  surfaces;
- semantic correspondence tags and localized Korean/English names/descriptions.

Each delivery includes actual editable sources where the acquisition contract
requires archival source, runtime modules, high/mobile variants, thumbnails
generated later, pack-source manifest, tool/export versions, creator handoff
notes, and asset hash matched by the legal evidence row. Update a matrix row to
`technical-approved` only after Task 3 passes.

`render_avatar_review_sheets.ps1` compiles each exact pack with the release
compiler and renders:

- transparent, white, black, and complex broadcast backgrounds;
- front, three-quarter left/right, profile left/right, back or authored 2D
  angle bounds;
- neutral, blink, gaze, five vowels, smile, frown, surprise;
- family-specific ear/tail/muzzle/paw/wing/beak/mascot controls;
- each color/material channel, physics on/off, high/mobile LOD/atlas;
- 100%, 50%, 25%, and 256-pixel-tall broadcast views.

Every sheet embeds catalog ID, runtime hash, compiler/renderer versions, camera,
motion fixture hash, palette, quality tier, and output pixel hash. It cannot
load marketing images.

Art director and rigging lead independently review the immutable sheet/source:
silhouette, face appeal, theme fidelity, color/contrast, line/material
consistency, seams, clipping, masks, deformation, extremes, physics,
independent controls, LOD transitions, and broadcast readability. Review JSON
records per-check pass/fail, actionable issue, reviewer, UTC, runtime/sheet hash,
and tool version. A revised asset invalidates prior reviews.

`CatalogSemanticAudit` validates all catalog/matrix/pack/review/hash links,
localized names/tags, all semantic tag equivalences, palette contrast, distinct
design roots, and approvals. Only rows with technical, art, rigging, and legal
approval become `approved`. It also writes
`content/avatar/catalog/approved-source-paths.txt` as sorted, repository-relative
literal paths for every approved preset, pack manifest, editable source, and
runtime payload. The audit rejects duplicates, whitespace, pathspec magic,
wildcards, absolute paths, `..`, and any path outside `content/avatar/presets/`
or `content/avatar/packs/`.

- [ ] **Step 4: Audit final approved counts and review integrity**

Run:

```powershell
pwsh -NoProfile -File scripts/render_avatar_review_sheets.ps1 -Preset windows-release -Matrix content/avatar/CONTENT_MATRIX.csv -Output build/avatar-review
cmake --build --preset windows-debug --target avatar-content-audit cs_avatar_content_tests
ctest --test-dir build/windows-debug -R "ContentMatrixAuditTest|CatalogSemanticAuditTest|Avatar2dContentAuditTest|Avatar3dContentAuditTest" --output-on-failure
build/windows-debug/tools/avatar-content/avatar-content-audit.exe catalog --matrix content/avatar/CONTENT_MATRIX.csv --catalog content/avatar/catalog/catalog-source.json --reviews content/avatar/reviews/review-index.json --bom legal/ASSET_BOM.csv --require-status approved
```

Expected: exactly linked immutable review evidence exists for every item; each
representation independently meets or exceeds 18 presets, 8 themes, 30 hair,
24 outfits, and 50 accessories with distinct approved design roots.

- [ ] **Step 5: Commit approved authored catalog sources and review index**

```powershell
git add content/avatar/ART_DIRECTION.md content/avatar/TECHNICAL_ART_STANDARD.md content/avatar/NAMING_AND_TAGGING.md content/avatar/CONTENT_MATRIX.csv content/avatar/catalog/catalog-source.json content/avatar/catalog/semantic-tag-map.json content/avatar/catalog/release-catalog.schema.json content/avatar/catalog/approved-source-paths.txt content/avatar/palettes/skin-and-marking-palettes.json content/avatar/reviews/visual-review.schema.json content/avatar/reviews/rig-review.schema.json content/avatar/reviews/review-index.json legal/ASSET_BOM.csv tools/avatar-content/CatalogSemanticAudit.h tools/avatar-content/CatalogSemanticAudit.cpp tools/avatar-content/VisualContactSheet.h tools/avatar-content/VisualContactSheet.cpp scripts/render_avatar_review_sheets.ps1 tests/avatar_content/CatalogSemanticAuditTest.cpp tools/avatar-content/CMakeLists.txt tests/CMakeLists.txt
git add --pathspec-from-file=content/avatar/catalog/approved-source-paths.txt
git commit -m "feat(avatar): add approved commercial launch catalog"
```

## Task 5: Exercise compatible combinations, extremes, and visual regressions

**Files:**
- Create: `tools/avatar-content/CombinationQualification.h`
- Create: `tools/avatar-content/CombinationQualification.cpp`
- Create: `tests/avatar_content/CombinationQualificationTest.cpp`
- Create: `tests/acceptance/CommercialAvatarCatalogAcceptanceTest.cpp`
- Modify: `tools/avatar-content/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar_content {

struct CombinationQualificationConfig {
    std::uint64_t seed;
    std::uint32_t randomizedCombinationsPerFamily{10000};
    std::vector<float> morphSamples{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
    std::vector<avatar::AvatarQualityTier> qualityTiers;
};

struct CombinationIssue {
    std::string specSha256;
    avatar::RigFamily family;
    avatar::AvatarRepresentation representation;
    std::vector<avatar::AvatarAssetId> assets;
    std::string code;
    std::string evidencePath;
};

} // namespace creator::avatar_content
```

- [ ] **Step 1: Add failing pairwise/random/extreme tests**

```cpp
TEST(CombinationQualificationTest, CoversEveryCompatiblePairAndMorphBoundary) {
    const auto plan = buildPlan(approvedCatalog(), defaultConfig());
    EXPECT_TRUE(everyCompatibleSlotPairAppears(plan));
    EXPECT_TRUE(everyAssetAppearsAtEachSupportedMorphBoundary(plan));
    EXPECT_GE(randomCountPerFamily(plan), 10000U);
}

TEST(CommercialAvatarCatalogAcceptanceTest, EveryApprovedAssetCompilesAndChangesPixels) {
    for (const auto& asset : approvedCatalog().entries()) {
        const auto result = qualifyAssetInRepresentativeSpec(asset);
        ASSERT_TRUE(result.hasValue()) << asset.id << ": "
                                       << result.error().message();
        EXPECT_TRUE(result.value().selectedAssetChangesExpectedPixels);
    }
}
```

- [ ] **Step 2: Run and prove combination qualification is absent**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_content_tests cs_commercial_avatar_catalog_acceptance_tests
ctest --test-dir build/windows-debug -R "CombinationQualificationTest|CommercialAvatarCatalogAcceptanceTest" --output-on-failure
```

Expected: coverage planner and final-catalog acceptance target are absent.

- [ ] **Step 3: Implement deterministic combinatorial and golden validation**

Build a deterministic covering array containing every compatible pair of slots,
every asset at neutral and supported morph bounds, every dependency/conflict,
every theme demonstration look, every family-specific control, every palette
surface, every representation, and every quality tier. Add 10,000 seeded valid
random combinations per family/representation; randomization must call the
product compatibility/rights selector rather than hand-assembling specs.

For each spec:

1. validate catalog/rights/compatibility;
2. compile through the release compiler and verify artifact/cache determinism;
3. render neutral, expression/body/family motion and physics frames;
4. verify real foreground/transparent alpha, finite geometry, no missing
   textures/materials, expected control pixel differences, and no renderer
   diagnostic;
5. measure 2D empty pixels/tears and mask boundaries; measure 3D intersections
   for declared garment/body regions and camera-near clipping;
6. render broadcast-scale/readability and high/mobile LOD transition frames;
7. compare invariant regions and perceptual golden thresholds, storing exact
   before/after evidence for failures.

Automated penetration thresholds are zero for undeclared body exposure through
garments and at most 0.5% screen pixels for authored soft-layer contacts.
Golden updates require the affected asset review to be rerun and cannot be
accepted by changing thresholds.

- [ ] **Step 4: Run complete catalog combination acceptance**

Run:

```powershell
cmake --build --preset windows-release --target avatar-content-audit cs_commercial_avatar_catalog_acceptance_tests
ctest --test-dir build/windows-release -R "CombinationQualificationTest|CommercialAvatarCatalogAcceptanceTest" --output-on-failure
build/windows-release/tools/avatar-content/avatar-content-audit.exe combinations --catalog content/avatar/catalog/catalog-source.json --seed 20260724 --random-per-family 10000 --output build/content-audit/combinations
```

Expected: all approved items compile in representative and boundary
combinations, family controls alter real pixels, thresholds pass, and every
failure includes a reproducible spec hash/seed/evidence render.

- [ ] **Step 5: Commit the combinatorial release gate**

```powershell
git add tools/avatar-content/CombinationQualification.h tools/avatar-content/CombinationQualification.cpp tests/avatar_content/CombinationQualificationTest.cpp tests/acceptance/CommercialAvatarCatalogAcceptanceTest.cpp tools/avatar-content/CMakeLists.txt tests/CMakeLists.txt
git commit -m "test(avatar): qualify launch catalog combinations"
```

## Task 6: Build signed immutable release packs and catalog

**Files:**
- Create: `tools/avatar-content/ReleaseCatalogBuilder.h`
- Create: `tools/avatar-content/ReleaseCatalogBuilder.cpp`
- Create: `scripts/build_avatar_release_catalog.ps1`
- Create: `tests/avatar_content/ReleaseCatalogBuilderTest.cpp`
- Modify: `tools/avatar-content/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::avatar_content {

struct ReleaseCatalogBuildRequest {
    std::filesystem::path matrixPath;
    std::filesystem::path catalogSourcePath;
    std::filesystem::path packSourceRoot;
    std::filesystem::path reviewIndexPath;
    std::filesystem::path assetBomPath;
    std::filesystem::path outputRoot;
    std::string releaseVersion;
    std::string sourceCommit;
    std::string signingKeyId;
};

class ReleaseCatalogBuilder final {
public:
    core::Result<ReleaseCatalogReceipt> build(
        const ReleaseCatalogBuildRequest& request,
        avatar::IAvatarPackageSigner& signer) const;
};

} // namespace creator::avatar_content
```

- [ ] **Step 1: Add failing approval/hash/signing tests**

```cpp
TEST(ReleaseCatalogBuilderTest, RejectsAnyUnapprovedOrChangedInput) {
    EXPECT_EQ(build(matrixWithOneTechnicalApproved()).error().code(),
              core::ErrorCode::InvalidState);
    EXPECT_EQ(build(afterChangingApprovedTextureByte()).error().code(),
              core::ErrorCode::IoFailure);
}

TEST(ReleaseCatalogBuilderTest, ProducesReproducibleSignedPacksAndIndex) {
    const auto first = build(validRequest());
    const auto second = build(validRequestInDifferentOutputRoot());
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().catalogSha256, second.value().catalogSha256);
    EXPECT_EQ(first.value().packHashes, second.value().packHashes);
    EXPECT_TRUE(verifyAllSignatures(first.value()));
}
```

- [ ] **Step 2: Run and prove release builder tests fail**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_content_tests
ctest --test-dir build/windows-debug -R ReleaseCatalogBuilderTest --output-on-failure
```

Expected: release builder and script are absent.

- [ ] **Step 3: Implement approval-locked deterministic packaging**

Builder requires `approved` matrix status and matching technical/art/rig/legal
hashes. It re-runs schema, path, dependency, signature-input, rights, count,
semantic, and budget audits before reading pack contents. It refuses a dirty
source asset path or any file not listed in `pack-source.json`.

For each pack it:

- normalizes manifest JSON and archive ordering/timestamps/permissions;
- includes only declared runtime/source-if-licensed, thumbnail, license/notice,
  catalog, and validation-report files;
- verifies compressed/uncompressed limits and path safety;
- signs the canonical manifest with the release Ed25519 service/HSM key ID;
- writes SHA-256, signature, public key ID, review IDs, tool versions, and
  rights evidence IDs;
- installs into a temporary catalog and compiles/renders one smoke spec.

The catalog index contains all pack IDs/versions/hashes/URLs, semantic metadata,
localized text, rights summary, compatibility, minimum app version, release
version, source commit, and its own signature. Signing secrets never enter
command arguments, logs, repository, or output.

`build_avatar_release_catalog.ps1` requires a clean source commit, release
preset, explicit output under `build/release/avatar/<version>`, and signing
service configuration. `-UnsignedTest` is accepted only for unit fixtures and
marks the receipt non-releasable.

- [ ] **Step 4: Build, reinstall, and verify the release catalog**

Run:

```powershell
cmake --build --preset windows-release --target avatar-content-audit
pwsh -NoProfile -File scripts/build_avatar_release_catalog.ps1 -Preset windows-release -Version 1.0.0 -Output build/release/avatar/1.0.0
build/windows-release/tools/avatar-content/avatar-content-audit.exe installed-catalog --root build/release/avatar/1.0.0 --require-signatures
ctest --test-dir build/windows-release -R ReleaseCatalogBuilderTest --output-on-failure
```

Expected: signed packs/index are deterministic, reinstall safely, resolve every
entry, compile smoke specs, and match all approved source/review/rights hashes.

- [ ] **Step 5: Commit deterministic release tooling**

```powershell
git add tools/avatar-content/ReleaseCatalogBuilder.h tools/avatar-content/ReleaseCatalogBuilder.cpp scripts/build_avatar_release_catalog.ps1 tests/avatar_content/ReleaseCatalogBuilderTest.cpp tools/avatar-content/CMakeLists.txt tests/CMakeLists.txt
git commit -m "build(avatar): produce signed immutable release catalog"
```

## Task 7: Qualify exact release builds on all physical platform tiers

**Files:**
- Create: `schemas/avatar-qualification-result.schema.json`
- Create: `scripts/run_avatar_device_qualification.ps1`
- Create: `tests/acceptance/AvatarCrossPlatformAcceptanceTest.cpp`
- Create: `release/avatar/qualification/README.md`
- Create: `release/avatar/qualification/1.0.0/device-summary.json`
- Create: `release/avatar/qualification/1.0.0/device-summary.ed25519`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace creator::qualification {

struct AvatarDeviceQualificationResult {
    std::uint32_t schemaVersion;
    std::string runId;
    std::string sourceCommit;
    std::string buildManifestSha256;
    std::string catalogSha256;
    std::string platform;
    std::string deviceTier;
    std::string physicalDeviceIdentity;
    std::string osVersion;
    std::string gpuAndDriver;
    std::uint32_t targetWidth;
    std::uint32_t targetHeight;
    double targetFps;
    double achievedFpsP50;
    double achievedFpsP95;
    double motionToPreviewLatencyP95Ms;
    double droppedFrameRatio;
    std::uint64_t peakResidentBytes;
    std::string status;
    std::vector<std::string> artifactHashes;
};

} // namespace creator::qualification
```

- [ ] **Step 1: Add failing device identity and threshold tests**

```cpp
TEST(AvatarCrossPlatformAcceptanceTest, RejectsUnmatchedOrEmulatedHardware) {
    EXPECT_EQ(validate(resultFromSoftwareGpu()).error().code(),
              core::ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(resultClaimingM1OnM2()).error().code(),
              core::ErrorCode::InvalidArgument);
}

TEST(AvatarCrossPlatformAcceptanceTest, EnforcesExactResolutionFpsAndLatency) {
    auto windows = validWindowsResult();
    windows.motionToPreviewLatencyP95Ms = 80.01;
    EXPECT_EQ(validate(windows).error().code(),
              core::ErrorCode::InvalidState);
    auto android = validSnapdragonResult();
    android.targetWidth = 1920;
    EXPECT_EQ(validate(android).error().code(),
              core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run and prove platform qualification is absent**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_cross_platform_acceptance_tests
ctest --test-dir build/windows-debug -R AvatarCrossPlatformAcceptanceTest --output-on-failure
```

Expected: qualification schema, validator, harness, and results are absent.

- [ ] **Step 3: Implement reproducible physical-device scenarios**

`run_avatar_device_qualification.ps1` installs/launches the exact release build
and signed catalog, verifies build/catalog hashes, rejects remote/software GPU
or emulators, gathers platform identity, warms for five minutes, then runs this
matrix with fixed consented motion/audio:

| Scenario | Representation/family | Required edit/output |
|---|---|---|
| 2D human | Inochi2D humanoid | face/body/hair/outfit/material edit, tracking, transparent preview/record |
| 2D animal | Inochi2D quadruped or avian | family controls, physics, tracking, chroma/record |
| 3D human | VRM humanoid | 360°, morph/material/outfit, tracking, alpha/record |
| 3D animal | glTF quadruped or avian | true skeleton, paws/wings/beak/tail, tracking, chroma/record |

It also creates/reopens one preset per family, runs representative catalog
edits, imports/exports allowed fixtures, proves denied exports create no file,
tests missing-asset repair, Korean/English, keyboard/mouse/touch as applicable,
and captures screenshots/frame hashes from actual runtime surfaces.

Measurement uses capture timestamp at motion input and presentation timestamp
at preview with synchronized monotonic clocks. Record:
tracking/render/presentation p50/p95/p99, achieved/dropped frames, queue depths,
CPU/GPU/memory/temperature, quality tier changes, output hashes, alpha/chroma,
and A/V sync. Targets:

| Tier | Output | Minimum achieved rate | p95 motion-to-preview |
|---|---|---:|---:|
| Windows RTX 3060 | 1920×1080 | 60.0 fps | ≤80 ms |
| macOS Apple M1 | 1920×1080 | 60.0 fps | ≤80 ms |
| Snapdragon 8 Gen 1 | 1280×720 | 30.0 fps | ≤120 ms |
| Higher Android | 1920×1080 | 30.0 fps | ≤120 ms |

Dropped-render ratio must be ≤0.5% after warm-up, output timestamp order must be
strict, and quality-tier transitions must appear in results/UI. Thermal
throttling is measured, not hidden by shortening the scenario.

The harness writes schema-valid JSON plus raw timestamp CSV, logs, screenshots,
frame hashes, device report, and signed test-run receipt. It never turns
`not-qualified`, missing permissions, disconnected camera, or missing output
consumer into pass.

- [ ] **Step 4: Run and collect all four physical-tier results**

Run from the qualification coordinator with connected devices:

```powershell
pwsh -NoProfile -File scripts/run_avatar_device_qualification.ps1 -BuildRoot build/release/app/1.0.0 -CatalogRoot build/release/avatar/1.0.0 -Tier windows-rtx3060 -Output build/qualification/windows-rtx3060
pwsh -NoProfile -File scripts/run_avatar_device_qualification.ps1 -BuildRoot build/release/app/1.0.0 -CatalogRoot build/release/avatar/1.0.0 -Tier macos-m1 -Output build/qualification/macos-m1
pwsh -NoProfile -File scripts/run_avatar_device_qualification.ps1 -BuildRoot build/release/app/1.0.0 -CatalogRoot build/release/avatar/1.0.0 -Tier android-sd8gen1 -Output build/qualification/android-sd8gen1
pwsh -NoProfile -File scripts/run_avatar_device_qualification.ps1 -BuildRoot build/release/app/1.0.0 -CatalogRoot build/release/avatar/1.0.0 -Tier android-high-1080p -Output build/qualification/android-high-1080p
```

Expected: four physical-tier result directories report `pass`, meet exact
resolution/rate/latency/drop thresholds, share release hashes, and contain all
scenario evidence. Any unavailable tier leaves this task incomplete.

- [ ] **Step 5: Commit the qualification harness and signed result manifest**

Aggregate the four schema-validated results and artifact hashes—not bulky raw
logs or personal capture data—into canonical
`release/avatar/qualification/1.0.0/device-summary.json`, then write its
detached signature to `device-summary.ed25519`.

```powershell
git add schemas/avatar-qualification-result.schema.json scripts/run_avatar_device_qualification.ps1 tests/acceptance/AvatarCrossPlatformAcceptanceTest.cpp release/avatar/qualification/README.md release/avatar/qualification/1.0.0/device-summary.json release/avatar/qualification/1.0.0/device-summary.ed25519 tests/CMakeLists.txt
git commit -m "test(avatar): qualify release on physical platform tiers"
```

## Task 8: Prove two-hour stability, recovery, synchronization, and resource bounds

**Files:**
- Create: `scripts/run_avatar_stability_qualification.ps1`
- Create: `tests/acceptance/AvatarLongRunAcceptanceTest.cpp`
- Create: `release/avatar/qualification/1.0.0/stability-summary.json`
- Create: `release/avatar/qualification/1.0.0/stability-summary.ed25519`
- Modify: `tests/CMakeLists.txt`
- Modify: `schemas/avatar-qualification-result.schema.json`

**Interfaces:**
- Extends each device result with scenario duration, warm/post memory, GPU
  allocations, device-loss/provider-restart/autosave recovery, A/V sync, and
  continuous output hashes.

- [ ] **Step 1: Add failing long-run threshold and failure-recovery tests**

```cpp
TEST(AvatarLongRunAcceptanceTest, RequiresTwoHoursForAllFourScenarios) {
    auto result = validLongRunResult();
    result.scenarios.at("3d-animal").durationSeconds = 7199;
    EXPECT_EQ(validate(result).error().code(),
              core::ErrorCode::InvalidState);
}

TEST(AvatarLongRunAcceptanceTest, RejectsLeaksSyncDriftAndHiddenRecoveryFailure) {
    auto result = validLongRunResult();
    result.postWarmupResidentGrowthBytes = 129ULL * 1024ULL * 1024ULL;
    EXPECT_EQ(validate(result).error().code(),
              core::ErrorCode::InvalidState);
    result = validLongRunResult();
    result.avSyncP95Ms = 20.01;
    EXPECT_EQ(validate(result).error().code(),
              core::ErrorCode::InvalidState);
}
```

- [ ] **Step 2: Run and prove stability validation fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_avatar_long_run_acceptance_tests
ctest --test-dir build/windows-debug -R AvatarLongRunAcceptanceTest --output-on-failure
```

Expected: long-run validator and harness are absent.

- [ ] **Step 3: Implement deterministic two-hour stress and failure injection**

For each physical tier and each of the four scenarios, run 7,200 continuous
seconds after five-minute warm-up with looping consented motion/audio, active
tracking, real renderer, Studio composition, recording, and alpha/chroma
broadcast consumer. Rotate compatible parts/colors/materials/physics every five
minutes through real editor commands and autosave/reopen one checkpoint.

Inject at fixed recorded times:

- camera permission/source loss and recovery;
- MediaPipe failure followed by limited restart then audio fallback;
- broadcast consumer disconnect/reconnect;
- one supported GPU-device/context recreation;
- autosave write failure then explicit retry;
- unavailable asset in a copied recovery project;
- Android background/foreground and orientation;
- desktop window minimize/restore and display scale change.

Pass criteria after warm-up:

- 7,200 seconds per scenario with no crash/hang/unhandled exception;
- dropped render frames ≤0.5%, strictly monotonic timestamps;
- p95 latency remains within tier budget in each 15-minute window;
- p95 A/V sync absolute error ≤20 ms and final drift ≤10 ms;
- resident memory growth ≤128 MiB desktop and ≤64 MiB Android;
- live GPU resource counts return within 2% of post-warm baseline after each
  edit cycle and zero after shutdown;
- no continuously growing queue, descriptor, handle, thread, texture, or
  temporary file count;
- fallback/reacquire follows documented timings; GPU recovery ≤3 seconds or
  explicitly enters safe quality with visible status;
- autosave last-good recovers identical spec/artifact; missing asset is not
  substituted;
- recording/broadcast frame provenance remains the active release artifact.

`run_avatar_stability_qualification.ps1` requires AC power except the explicit
Android thermal segment, prevents sleep, checks free storage, samples metrics
every second, keeps raw artifacts outside Git, and writes a signed summary.
Interrupted or manually shortened runs fail.

- [ ] **Step 4: Execute sixteen required long runs**

Run once per tier; each invocation executes the four two-hour scenarios:

```powershell
pwsh -NoProfile -File scripts/run_avatar_stability_qualification.ps1 -Tier windows-rtx3060 -DurationSeconds 7200 -Output build/qualification/stability/windows-rtx3060
pwsh -NoProfile -File scripts/run_avatar_stability_qualification.ps1 -Tier macos-m1 -DurationSeconds 7200 -Output build/qualification/stability/macos-m1
pwsh -NoProfile -File scripts/run_avatar_stability_qualification.ps1 -Tier android-sd8gen1 -DurationSeconds 7200 -Output build/qualification/stability/android-sd8gen1
pwsh -NoProfile -File scripts/run_avatar_stability_qualification.ps1 -Tier android-high-1080p -DurationSeconds 7200 -Output build/qualification/stability/android-high-1080p
```

Expected: all sixteen scenario runs pass duration, latency, frame, memory/GPU,
sync, recovery, provenance, and shutdown checks. Any interrupted or missing run
leaves release blocked.

- [ ] **Step 5: Commit the stability gate and signed summaries**

Aggregate all sixteen validated runs into canonical
`release/avatar/qualification/1.0.0/stability-summary.json` and sign the exact
bytes as `stability-summary.ed25519`; raw logs remain outside Git.

```powershell
git add scripts/run_avatar_stability_qualification.ps1 tests/acceptance/AvatarLongRunAcceptanceTest.cpp tests/CMakeLists.txt schemas/avatar-qualification-result.schema.json release/avatar/qualification/1.0.0/stability-summary.json release/avatar/qualification/1.0.0/stability-summary.ed25519
git commit -m "test(avatar): prove two hour release stability"
```

## Task 9: Aggregate one non-bypassable commercial launch verdict

**Files:**
- Create: `scripts/audit_avatar_content.ps1`
- Create: `scripts/verify_avatar_release.ps1`
- Create: `tests/scripts/AvatarReleaseVerdictTest.ps1`
- Create: `release/avatar/qualification/1.0.0/qualification-verdict.json`
- Create: `release/avatar/qualification/1.0.0/qualification-verdict.ed25519`
- Modify: `release/avatar/qualification/README.md`
- Modify: `README.md`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `build/release/avatar/<version>/release-manifest.json`
  - `build/release/avatar/<version>/qualification-verdict.json`
  - process exit 0 only for verdict `pass`.

- [ ] **Step 1: Add the failing missing-gate aggregation test**

```powershell
$required = @(
  'content-matrix', 'asset-rights', 'technical-2d', 'technical-3d',
  'art-review', 'rig-review', 'catalog-semantics', 'combinations',
  'package-signatures', 'windows-device', 'macos-device',
  'android-sd8gen1-device', 'android-high-device',
  'windows-stability', 'macos-stability',
  'android-sd8gen1-stability', 'android-high-stability',
  'qml-connections', 'accessibility', 'localization',
  'workspace-output', 'full-regression'
)
foreach ($gate in $required) {
  $fixture = New-VerdictFixture -Missing $gate
  & scripts/verify_avatar_release.ps1 -EvidenceRoot $fixture
  if ($LASTEXITCODE -eq 0) { throw "Missing gate accepted: $gate" }
}
```

- [ ] **Step 2: Run and prove the final verifier is absent**

Run:

```powershell
pwsh -NoProfile -File tests/scripts/AvatarReleaseVerdictTest.ps1
```

Expected: verifier or verdict test is absent.

- [ ] **Step 3: Implement exact evidence aggregation and anti-substitute scan**

`audit_avatar_content.ps1` runs matrix, rights, technical, semantic, review,
combination, package, and installed-catalog audits against one source commit and
catalog hash. `verify_avatar_release.ps1` then:

1. requires a clean signed release build and signed catalog;
2. verifies every evidence schema, signature, hash, source commit, build
   manifest, catalog, device identity, duration, and status;
3. rejects duplicate run IDs, expired rights, mismatched assets, stale reviews,
   absent tiers/scenarios, `not-qualified`, warning-as-pass, or threshold
   rounding;
4. confirms the approved counts independently for 2D/3D and all families/themes;
5. scans shipped catalog/runtime/QML resources for forbidden development asset
   classes and names such as test-only shapes, stock preview screenshots,
   sample/demo/test-pattern/watermark markers, and asset IDs absent from the
   approved matrix;
6. compares thumbnail provenance against runtime hashes and rejects an image
   with no corresponding real model;
7. requires the QML connection audit, accessibility/localization tests, real
   workspace/output acceptance, full-motion acceptance, all unit/integration
   tests, and zero `git diff --check` errors;
8. emits one canonical verdict with every input digest and exits non-zero unless
   every gate is `pass`.

The verifier has no `--skip`, `--force`, `--accept-warning`, or manual-pass
option. Rerunning the failed underlying gate is the only way to obtain pass.

- [ ] **Step 4: Run the complete release gate**

Run:

```powershell
cmake --build --preset windows-release
ctest --test-dir build/windows-release --output-on-failure
pwsh -NoProfile -File scripts/audit_avatar_qml_connections.ps1
pwsh -NoProfile -File scripts/audit_avatar_content.ps1 -BuildRoot build/windows-release -CatalogRoot build/release/avatar/1.0.0 -Output build/qualification/content
pwsh -NoProfile -File scripts/verify_avatar_release.ps1 -Version 1.0.0 -BuildRoot build/release/app/1.0.0 -CatalogRoot build/release/avatar/1.0.0 -EvidenceRoot build/qualification -Output build/release/avatar/1.0.0
git diff --check
```

Expected: every automated, human-review, legal, physical-device, long-run,
connected-output, accessibility/localization, and regression gate reports
`pass`; the final verdict and release manifest are signed and internally
hash-consistent.

- [ ] **Step 5: Commit the launch verifier and release documentation**

Copy the canonical verified verdict from
`build/release/avatar/1.0.0/qualification-verdict.json` to the exact release
path below and sign its bytes before staging.

```powershell
git add scripts/audit_avatar_content.ps1 scripts/verify_avatar_release.ps1 tests/scripts/AvatarReleaseVerdictTest.ps1 release/avatar/qualification/README.md release/avatar/qualification/1.0.0/qualification-verdict.json release/avatar/qualification/1.0.0/qualification-verdict.ed25519 README.md tests/CMakeLists.txt
git commit -m "release(avatar): enforce commercial launch qualification"
```

## Plan Completion Gate

- Both 2D and 3D independently ship at least 18 distinct final editable presets,
  all eight themes, 30 hair/head designs, 24 outfit sets, and 50 accessories.
- Every bundled asset has model-specific commercial broadcast, corporate use,
  app-bundle, and derivative-character evidence tied to its exact hash.
- Real source/runtime assets pass deformation, customization, rig-family,
  LOD/atlas, memory, material, physics, and thumbnail provenance audits.
- Art and rigging reviews are immutable-hash-specific and cover real runtime
  contact sheets at broadcast size, angles, expressions, family motion,
  backgrounds, and quality tiers.
- Deterministic pairwise, morph-boundary, family, theme, and 10,000-random-
  combination-per-family tests pass without static, rigid, duplicate, or
  disconnected substitutes.
- Signed packs and catalog are deterministic, installable, rights-consistent,
  and built only from approved hashes.
- Exact release builds pass Windows RTX 3060, Apple M1, Snapdragon 8 Gen 1, and
  higher-Android physical-device output/latency gates.
- Sixteen required two-hour scenario runs pass stability, synchronization,
  memory/GPU, failure recovery, autosave, and output-provenance criteria.
- One non-bypassable verifier rejects missing/stale/mismatched evidence and emits
  `pass` only when the whole commercially usable product is genuinely qualified.
