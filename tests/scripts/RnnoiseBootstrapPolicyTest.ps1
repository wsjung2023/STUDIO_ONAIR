[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = "Stop"
$BootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_rnnoise.ps1"
$VerifierPath = Join-Path $RepositoryRoot "scripts/verify_rnnoise_runtime.ps1"

foreach ($Required in @($BootstrapPath, $VerifierPath)) {
    if (-not (Test-Path -LiteralPath $Required)) {
        throw "Missing RNNoise policy artifact: $Required"
    }
}

# --- Bootstrap: pin + REAL SHA + official source + verification present ------
$Bootstrap = Get-Content -LiteralPath $BootstrapPath -Raw -Encoding utf8
$RequiredPatterns = @(
    '0\.1\.1',
    '6cbfd53eb348a8d394e0757b4025c6ded34eb2b6',
    '1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987',
    'ExpectedSourceArchiveSha256',
    'github\.com/xiph/rnnoise',
    'Get-FileHash',
    'verify_rnnoise_runtime\.ps1',
    'rnnoise-runtime-manifest\.json',
    'creator-studio-rnnoise-build\.txt',
    'BSD-3-Clause'
)
foreach ($Pattern in $RequiredPatterns) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "RNNoise bootstrap is missing required policy evidence: $Pattern"
    }
}

# --- Bootstrap: forbidden GPL / nonfree configuration must be absent ---------
$ForbiddenPatterns = @(
    '--enable-gpl',
    '--enable-nonfree',
    'GPL=ON',
    'GPL3=ON',
    '--enable-x264',
    '--enable-x265',
    '--enable-fdk-aac'
)
foreach ($Pattern in $ForbiddenPatterns) {
    if ($Bootstrap -match $Pattern) {
        throw "RNNoise bootstrap contains forbidden configuration: $Pattern"
    }
}

# --- Verifier: fail-closed identity + hash + forbidden checks ----------------
$Verifier = Get-Content -LiteralPath $VerifierPath -Raw -Encoding utf8
foreach ($Pattern in @(
    'SHA256',
    'source_commit',
    'source_archive_sha256',
    '0\.1\.1',
    'BSD-3-Clause',
    'static',
    'forbidden',
    'unexpected'
)) {
    if ($Verifier -notmatch $Pattern) {
        throw "RNNoise verifier is missing fail-closed check: $Pattern"
    }
}

# --- CMake gate: default OFF, FATAL_ERROR guard, runtime verification --------
$CMake = Get-Content -LiteralPath (Join-Path $RepositoryRoot "CMakeLists.txt") `
    -Raw -Encoding utf8
if ($CMake -notmatch 'option\(CS_ENABLE_RNNOISE\s+"[^"]*"\s+OFF\)') {
    throw "CS_ENABLE_RNNOISE gate must be declared and default to OFF"
}
foreach ($Pattern in @(
    'CS_ENABLE_RNNOISE requires CS_RNNOISE_ROOT',
    'verify_rnnoise_runtime\.ps1',
    'add_subdirectory\(src/rnnoise_adapter\)'
)) {
    if ($CMake -notmatch $Pattern) {
        throw "Root CMake is missing the RNNoise gate boundary: $Pattern"
    }
}

# --- Adapter sources present -------------------------------------------------
foreach ($Required in @(
    "src/rnnoise_adapter/CMakeLists.txt",
    "src/rnnoise_adapter/RnnoiseRuntimeManifest.h",
    "src/rnnoise_adapter/RnnoiseRuntimeManifest.cpp",
    "src/rnnoise_adapter/RnnoiseDenoiseProcessor.h",
    "src/rnnoise_adapter/RnnoiseDenoiseProcessor.cpp",
    "src/audio_dsp/UnavailableDenoiseProcessor.h",
    "src/audio_dsp/DenoiseProcessorFactory.h"
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepositoryRoot $Required))) {
        throw "Missing RNNoise adapter source: $Required"
    }
}

# --- OSS BOM: approved RNNoise row with pin + SHA ----------------------------
$Bom = Get-Content -LiteralPath (Join-Path $RepositoryRoot "legal/OSS_BOM.csv") `
    -Raw -Encoding utf8
$RnnoiseRow = ($Bom -split "\r?\n" | Where-Object { $_ -match '^RNNoise,' })
if (-not $RnnoiseRow) {
    throw "legal/OSS_BOM.csv is missing the RNNoise row"
}
foreach ($Pattern in @(
    'BSD-3-Clause',
    'APPROVED',
    '6cbfd53eb348a8d394e0757b4025c6ded34eb2b6',
    '1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987'
)) {
    if ($RnnoiseRow -notmatch $Pattern) {
        throw "RNNoise OSS_BOM row is missing required evidence: $Pattern"
    }
}

Write-Host "RNNoise bootstrap policy is fail-closed."
