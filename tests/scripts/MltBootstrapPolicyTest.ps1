[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = "Stop"
$FfmpegBootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_ffmpeg.ps1"
$BootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_mlt.ps1"
$VerifierPath = Join-Path $RepositoryRoot "scripts/verify_mlt_runtime.ps1"
$StagePath = Join-Path $RepositoryRoot "scripts/stage_mlt_runtime.ps1"

if (-not (Test-Path -LiteralPath $FfmpegBootstrapPath)) {
    throw "Missing FFmpeg bootstrap: $FfmpegBootstrapPath"
}
if (-not (Test-Path -LiteralPath $BootstrapPath)) {
    throw "Missing MLT bootstrap: $BootstrapPath"
}
if (-not (Test-Path -LiteralPath $VerifierPath)) {
    throw "Missing MLT runtime verifier: $VerifierPath"
}
if (-not (Test-Path -LiteralPath $StagePath)) {
    throw "Missing MLT runtime staging script: $StagePath"
}

$FfmpegBootstrap = Get-Content -LiteralPath $FfmpegBootstrapPath -Raw -Encoding utf8
foreach ($Pattern in @(
    'ffmpeg\[avcodec,avdevice,avfilter,avformat,ffprobe,swresample,swscale,zlib\]',
    '--enable-zlib',
    '-decoders',
    'png_decoder=enabled'
)) {
    if ($FfmpegBootstrap -notmatch $Pattern) {
        throw "FFmpeg bootstrap is missing PNG/zlib policy evidence: $Pattern"
    }
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
foreach ($Pattern in @('SHA256', 'unexpected', 'forbidden', 'source_commit', '7\.40\.0', 'zlib', 'Zlib')) {
    if ($Verifier -notmatch $Pattern) {
        throw "MLT verifier is missing fail-closed check: $Pattern"
    }
}
if ($Bootstrap -notmatch 'Copy-RequiredFile[^\r\n]*bin/z\.dll' -or
    $Verifier -notmatch '"bin/z\.dll"') {
    throw "MLT runtime policy must package and verify the vcpkg z.dll name"
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
$MltEngine = Get-Content -LiteralPath `
    (Join-Path $RepositoryRoot "src/mlt_adapter/MltEditEngine.cpp") `
    -Raw -Encoding utf8
foreach ($Pattern in @(
    'stage_mlt_runtime\.ps1',
    'DELAYLOAD:mlt\+\+-7\.dll',
    'DELAYLOAD:mlt-7\.dll',
    'DESTINATION bin/mlt-runtime'
)) {
    if ($CMake -notmatch $Pattern) {
        throw "Shipping CMake is missing the isolated MLT runtime boundary: $Pattern"
    }
}
if ($CMake -match 'CS_APP_MLT_ROOT') {
    throw "Shipping CMake must not compile a development-machine MLT path into the app"
}
foreach ($Pattern in @('applicationDirPath', 'mlt-runtime')) {
    if ($Main -notmatch $Pattern) {
        throw "Application startup is missing the staged MLT runtime boundary: $Pattern"
    }
}
foreach ($Pattern in @('AddDllDirectory', 'LoadLibraryExW', 'verifyMltRuntimeManifest')) {
    if ($MltEngine -notmatch $Pattern) {
        throw "MLT adapter is missing its verified delay-load boundary: $Pattern"
    }
}

Write-Host "MLT bootstrap policy is fail-closed."
