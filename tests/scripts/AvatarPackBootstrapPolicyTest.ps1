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
    $BomPath
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
    'find_package\(Sodium REQUIRED\)',
    'add_subdirectory\(src/avatar_pack_adapter\)'
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
    'source_url'
)) {
    if ($Verifier -notmatch $Pattern) {
        throw "libsodium verifier is missing a fail-closed check: $Pattern"
    }
}

$Finder = Get-Content -LiteralPath $FinderPath -Raw -Encoding utf8
foreach ($Pattern in @('Sodium::Sodium', 'sodium\.h', 'libsodium')) {
    if ($Finder -notmatch $Pattern) {
        throw "FindSodium.cmake is missing imported target policy: $Pattern"
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
