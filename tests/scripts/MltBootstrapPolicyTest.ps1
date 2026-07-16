[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = "Stop"
$BootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_mlt.ps1"
$VerifierPath = Join-Path $RepositoryRoot "scripts/verify_mlt_runtime.ps1"
$StagePath = Join-Path $RepositoryRoot "scripts/stage_mlt_runtime.ps1"

if (-not (Test-Path -LiteralPath $BootstrapPath)) {
    throw "Missing MLT bootstrap: $BootstrapPath"
}
if (-not (Test-Path -LiteralPath $VerifierPath)) {
    throw "Missing MLT runtime verifier: $VerifierPath"
}
if (-not (Test-Path -LiteralPath $StagePath)) {
    throw "Missing MLT runtime staging script: $StagePath"
}

$Bootstrap = Get-Content -LiteralPath $BootstrapPath -Raw -Encoding utf8
$RequiredPatterns = @(
    '7\.40\.0',
    'bef9d89c0c279e558d9625dac3399c2aa3d961bc',
    'ExpectedSourceArchiveSha256',
    'GPL=OFF',
    'GPL3=OFF',
    'MOD_AVFORMAT=ON',
    'USE_AVDEVICE=OFF',
    'USE_LV2=OFF',
    'USE_VST2=OFF',
    'VCPKG_MANIFEST_MODE=OFF',
    'MOD_PLUS=OFF',
    'MOD_QT6=OFF',
    'MOD_PLUSGPL=OFF',
    'MOD_XML=OFF',
    'mlt-runtime-manifest\.json',
    'Get-FileHash',
    'creator-studio-mlt-build\.txt'
)
foreach ($Pattern in $RequiredPatterns) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "MLT bootstrap is missing required policy evidence: $Pattern"
    }
}

$ForbiddenPackagingPatterns = @(
    'cmake\s+--install',
    'Copy-Item[^\r\n]*melt(?:\.exe)?',
    'MOD_PLUSGPL=ON',
    'GPL=ON',
    'GPL3=ON'
)
foreach ($Pattern in $ForbiddenPackagingPatterns) {
    if ($Bootstrap -match $Pattern) {
        throw "MLT bootstrap contains forbidden packaging/configuration: $Pattern"
    }
}

$Verifier = Get-Content -LiteralPath $VerifierPath -Raw -Encoding utf8
foreach ($Pattern in @('SHA256', 'unexpected', 'forbidden', 'source_commit', '7\.40\.0')) {
    if ($Verifier -notmatch $Pattern) {
        throw "MLT verifier is missing fail-closed check: $Pattern"
    }
}

$Stage = Get-Content -LiteralPath $StagePath -Raw -Encoding utf8
foreach ($Pattern in @(
    'runtime-library',
    'runtime-module',
    'runtime-data',
    'evidence',
    'development',
    'verify_mlt_runtime\.ps1',
    'mlt-runtime-manifest\.json',
    'ConvertTo-Json'
)) {
    if ($Stage -notmatch $Pattern) {
        throw "MLT staging script is missing runtime-only policy: $Pattern"
    }
}

$CMake = Get-Content -LiteralPath (Join-Path $RepositoryRoot "CMakeLists.txt") `
    -Raw -Encoding utf8
$Main = Get-Content -LiteralPath (Join-Path $RepositoryRoot "src/main.cpp") `
    -Raw -Encoding utf8
foreach ($Pattern in @(
    'stage_mlt_runtime\.ps1',
    'DELAYLOAD:mlt\+\+-7\.dll',
    'DELAYLOAD:mlt-7\.dll'
)) {
    if ($CMake -notmatch $Pattern) {
        throw "Shipping CMake is missing the isolated MLT runtime boundary: $Pattern"
    }
}
if ($CMake -match 'CS_APP_MLT_ROOT') {
    throw "Shipping CMake must not compile a development-machine MLT path into the app"
}
foreach ($Pattern in @('applicationDirPath', 'mlt-runtime', 'AddDllDirectory')) {
    if ($Main -notmatch $Pattern) {
        throw "Application startup is missing the staged MLT runtime boundary: $Pattern"
    }
}

Write-Host "MLT bootstrap policy is fail-closed."
