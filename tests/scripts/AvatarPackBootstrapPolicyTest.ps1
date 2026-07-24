[CmdletBinding()]
param(
    [string]$RepositoryRoot = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)

$RootCMakePath = Join-Path $RepositoryRoot "CMakeLists.txt"
$PresetPath = Join-Path $RepositoryRoot "CMakePresets.json"
$BootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_sodium.ps1"
$VerifierPath = Join-Path $RepositoryRoot "scripts/verify_sodium_runtime.ps1"
$FinderPath = Join-Path $RepositoryRoot "cmake/FindSodium.cmake"
$AdapterCMakePath = Join-Path $RepositoryRoot "src/avatar_pack_adapter/CMakeLists.txt"
$BomPath = Join-Path $RepositoryRoot "legal/OSS_BOM.csv"
$CanonicalTreePath = Join-Path $RepositoryRoot "scripts/canonical_tree_digest.ps1"
$MinizVerifierPath = Join-Path $RepositoryRoot "scripts/verify_miniz_source.ps1"
$RuntimeTrustTestPath = Join-Path $RepositoryRoot "tests/scripts/SodiumRuntimeTrustTest.ps1"
$CMakeTrustTestPath = Join-Path $RepositoryRoot "tests/scripts/AvatarPackCMakeTrustTest.ps1"
$CanonicalTestPath = Join-Path $RepositoryRoot "tests/scripts/CanonicalTreeDigestTest.ps1"
$PostConfigureAuditTestPath = Join-Path $RepositoryRoot `
    "tests/scripts/AvatarPackPostConfigureBuildAuditTest.ps1"
$TestsCMakePath = Join-Path $RepositoryRoot "tests/CMakeLists.txt"

$Text = ""
foreach ($Path in @($RootCMakePath, $BootstrapPath, $VerifierPath)) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $Text += Get-Content -LiteralPath $Path -Raw -Encoding utf8
    }
}

$Required = [ordered]@{
    "miniz" = "f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a"
    "libsodium" = "3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279"
}
foreach ($Pair in $Required.GetEnumerator()) {
    if ($Text -notmatch [regex]::Escape($Pair.Value)) {
        throw "$($Pair.Key) SHA-256 is not pinned"
    }
}

foreach ($Path in @(
    $RootCMakePath,
    $PresetPath,
    $BootstrapPath,
    $VerifierPath,
    $FinderPath,
    $AdapterCMakePath,
    $BomPath,
    $CanonicalTreePath,
    $MinizVerifierPath,
    $RuntimeTrustTestPath,
    $CMakeTrustTestPath,
    $CanonicalTestPath,
    $PostConfigureAuditTestPath,
    $TestsCMakePath
)) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing avatar pack dependency policy artifact: $Path"
    }
}

$RootCMake = Get-Content -LiteralPath $RootCMakePath -Raw -Encoding utf8
foreach ($Pattern in @(
    'option\(CS_ENABLE_AVATAR_PACKS "[^"]*" OFF\)',
    'set\(CS_SODIUM_ROOT "" CACHE PATH',
    'CS_ENABLE_AVATAR_PACKS requires CS_SODIUM_ROOT',
    'https://github\.com/richgel999/miniz/releases/download/3\.1\.2/miniz-3\.1\.2\.zip',
    'URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a',
    'FETCHCONTENT_SOURCE_DIR_MINIZ',
    'FETCHCONTENT_FULLY_DISCONNECTED',
    'FetchContent_GetProperties\(miniz\)',
    'verify_miniz_source\.ps1',
    'find_package\(Sodium REQUIRED\)',
    'add_custom_target\(cs_avatar_pack_dependency_audit',
    'add_dependencies\(miniz cs_avatar_pack_dependency_audit\)',
    'add_dependencies\(Sodium::Sodium cs_avatar_pack_dependency_audit\)',
    'add_subdirectory\(src/avatar_pack_adapter\)',
    'add_dependencies\(cs_avatar_pack_adapter cs_avatar_pack_dependency_audit\)'
)) {
    if ($RootCMake -notmatch $Pattern) {
        throw "Root CMake is missing avatar pack dependency policy: $Pattern"
    }
}

$Bootstrap = Get-Content -LiteralPath $BootstrapPath -Raw -Encoding utf8
foreach ($Pattern in @(
    '1\.0\.22',
    'https://download\.libsodium\.org/libsodium/releases/libsodium-1\.0\.22-msvc\.zip',
    'Get-FileHash',
    'runtime-manifest\.json',
    'source_url',
    'archive_sha256',
    'tree_sha256',
    '9e6dc4f9e295621388418ca22ee1ee3bbfb0632af1287a18ce91ad1842af22be',
    'include_path',
    'library_path',
    'dlls',
    'verify_sodium_runtime\.ps1'
)) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "libsodium bootstrap is missing required policy evidence: $Pattern"
    }
}

$Verifier = Get-Content -LiteralPath $VerifierPath -Raw -Encoding utf8
foreach ($Pattern in @(
    '1\.0\.22',
    'SHA256',
    'unexpected',
    'missing',
    'archive_sha256',
    'source_url',
    'tree_sha256',
    '9e6dc4f9e295621388418ca22ee1ee3bbfb0632af1287a18ce91ad1842af22be',
    'Get-CanonicalTreeDigest'
)) {
    if ($Verifier -notmatch $Pattern) {
        throw "libsodium verifier is missing a fail-closed check: $Pattern"
    }
}

$Finder = Get-Content -LiteralPath $FinderPath -Raw -Encoding utf8
foreach ($Pattern in @(
    'Sodium::Sodium',
    'sodium\.h',
    'libsodium',
    'REAL_PATH',
    'IS_PREFIX',
    'unset\("\$\{_cs_sodium_variable\}" CACHE\)'
)) {
    if ($Finder -notmatch $Pattern) {
        throw "FindSodium.cmake is missing imported target policy: $Pattern"
    }
}

$MinizVerifier = Get-Content -LiteralPath $MinizVerifierPath -Raw -Encoding utf8
foreach ($Pattern in @(
    '1638d4237f6a050f05f7e1eb5928d302916717a4f9c6ccb0d01e75735d512a76',
    'Get-CanonicalTreeDigest',
    'ExpectedSourceFileCount'
)) {
    if ($MinizVerifier -notmatch $Pattern) {
        throw "miniz verifier is missing canonical source policy: $Pattern"
    }
}

$CanonicalTree = Get-Content -LiteralPath $CanonicalTreePath -Raw -Encoding utf8
foreach ($Pattern in @(
    'StringComparer\]::Ordinal',
    'UTF8Encoding\(\$false\)',
    'WriteByte\(0\)',
    'ComputeHash',
    'ToArray',
    'GetFullPath',
    'Substring\(\$RootPrefix\.Length\)',
    '\\x00-\\x1F\\x7F',
    'FileAttributes\]::ReparsePoint'
)) {
    if ($CanonicalTree -notmatch $Pattern) {
        throw "Canonical tree serializer is missing required framing: $Pattern"
    }
}
if ($CanonicalTree -match 'System\.Uri|UnescapeDataString') {
    throw "Canonical tree serializer must preserve literal filesystem names"
}

$TrustTests = (Get-Content -LiteralPath $RuntimeTrustTestPath -Raw -Encoding utf8) +
    (Get-Content -LiteralPath $CMakeTrustTestPath -Raw -Encoding utf8) +
    (Get-Content -LiteralPath $CanonicalTestPath -Raw -Encoding utf8) +
    (Get-Content -LiteralPath $PostConfigureAuditTestPath -Raw -Encoding utf8)
foreach ($Pattern in @(
    'DLL plus manifest hash change',
    'header removal plus manifest list change',
    'import library plus manifest hash change',
    'literal percent header rename plus manifest path change',
    'Sodium_LIBRARY command-line cache escaped',
    'Stale Sodium cache paths survived',
    'FETCHCONTENT_SOURCE_DIR_MINIZ bypass was accepted',
    'FETCHCONTENT_FULLY_DISCONNECTED bypass was accepted',
    'Physical names A and %41 produced the same canonical digest',
    'accepted a reparse-point root',
    'Post-configure dependency mutations built successfully'
)) {
    if ($TrustTests -notmatch $Pattern) {
        throw "Avatar pack behavioral trust tests are missing: $Pattern"
    }
}

$TestsCMake = Get-Content -LiteralPath $TestsCMakePath -Raw -Encoding utf8
foreach ($Pattern in @(
    'NAME CanonicalTreeDigest',
    'NAME AvatarPackPostConfigureBuildAudit'
)) {
    if ($TestsCMake -notmatch $Pattern) {
        throw "Avatar pack trust regression is not registered with CTest: $Pattern"
    }
}

$AdapterCMake = Get-Content -LiteralPath $AdapterCMakePath -Raw -Encoding utf8
foreach ($Pattern in @('cs_avatar_pack_adapter', '\bminiz\b', 'Sodium::Sodium')) {
    if ($AdapterCMake -notmatch $Pattern) {
        throw "Avatar pack adapter target is missing dependency boundary: $Pattern"
    }
}

$Presets = Get-Content -LiteralPath $PresetPath -Raw -Encoding utf8
foreach ($Pattern in @(
    '"name": "windows-avatar-packs-debug"',
    '"CS_ENABLE_AVATAR_PACKS": "ON"',
    '"CS_SODIUM_ROOT": "\$\{sourceDir\}/build/sodium/prefix"'
)) {
    if ($Presets -notmatch $Pattern) {
        throw "CMake presets are missing the audited avatar pack preset: $Pattern"
    }
}

$Bom = Get-Content -LiteralPath $BomPath -Raw -Encoding utf8
foreach ($Pattern in @(
    '^miniz,.*3\.1\.2.*f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a',
    '^libsodium,.*1\.0\.22.*3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279'
)) {
    if ($Bom -notmatch "(?m)$Pattern") {
        throw "OSS BOM is missing avatar pack dependency evidence: $Pattern"
    }
}

Write-Host "Avatar pack bootstrap policy is fail-closed."
