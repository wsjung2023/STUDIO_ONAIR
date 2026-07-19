# Audited RNNoise (xiph/rnnoise, BSD-3-Clause) bootstrap.
#
# Mirrors scripts/bootstrap_ffmpeg.ps1 / bootstrap_mlt.ps1: pin a specific
# upstream release, download the OFFICIAL source archive, verify its real
# SHA-256 before anything is unpacked, build the library from that verified
# source (CPU only, no GPL / no nonfree), stage the static lib + public header
# into an audited prefix, and emit both human-readable build evidence and a
# machine-checkable runtime manifest that scripts/verify_rnnoise_runtime.ps1 and
# the C++ RnnoiseRuntimeManifest re-verify before the denoiser is ever used.
#
# RNNoise v0.1.1 ships the trained model compiled INTO the source
# (src/rnn_data.c, ~11k lines of baked weights) with no download_model.sh step,
# so unlike whisper there is NO separate model artifact to pin: pinning +
# hashing the single source archive fixes both the code and the weights.
[CmdletBinding()]
param(
    [string]$BuildRoot = "",
    [string]$InstallRoot = "",
    [string]$OfficialArchivePath = "",
    [ValidateSet("x64-windows")]
    [string]$Triplet = "x64-windows"
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# Pinned upstream identity. The archive SHA-256 is the REAL hash of the tag
# tarball (verified at authoring time); do not edit these without re-hashing.
$ExpectedRnnoiseVersion = "0.1.1"
$ExpectedSourceCommit = "6cbfd53eb348a8d394e0757b4025c6ded34eb2b6"
$ExpectedSourceArchiveSha256 = "1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987"
$OfficialSourceArchiveUrl = "https://github.com/xiph/rnnoise/archive/refs/tags/v$ExpectedRnnoiseVersion.tar.gz"
$ExpectedLicense = "BSD-3-Clause"

# Authoritative library source list from the pinned Makefile.am
# (librnnoise_la_SOURCES) — the model/weights (rnn_data.c) are part of it.
$LibrarySources = @(
    "src/denoise.c",
    "src/rnn.c",
    "src/rnn_data.c",
    "src/rnn_reader.c",
    "src/pitch.c",
    "src/kiss_fft.c",
    "src/celt_lpc.c"
)

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepositoryRoot "build/rnnoise"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $BuildRoot "prefix"
}
if ([string]::IsNullOrWhiteSpace($OfficialArchivePath)) {
    $OfficialArchivePath = Join-Path $RepositoryRoot "build/downloads/rnnoise-v$ExpectedRnnoiseVersion.tar.gz"
}
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$OfficialArchivePath = [System.IO.Path]::GetFullPath($OfficialArchivePath)
$SourceRoot = Join-Path $BuildRoot "source"
$NativeBuildRoot = Join-Path $BuildRoot "native-build"

# Reuse the MSVC x64 toolchain the way bootstrap_mlt.ps1 does, so cl/cmake/ninja
# resolve when this script is not run from a Developer PowerShell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    -not (Get-Command cmake.exe -ErrorAction SilentlyContinue) -or
    -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    $VsDevCmd = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $VsDevCmd)) {
        throw "Visual Studio 2022 x64 build environment was not found"
    }
    $EnvironmentLines = & cmd.exe /d /s /c "`"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "Could not initialize the MSVC x64 environment" }
    foreach ($Line in $EnvironmentLines) {
        $Separator = $Line.IndexOf('=')
        if ($Separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $Line.Substring(0, $Separator), $Line.Substring($Separator + 1),
                [EnvironmentVariableTarget]::Process)
        }
    }
}

# --- 1. Download + verify the official source archive -----------------------
if (-not (Test-Path -LiteralPath $OfficialArchivePath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OfficialArchivePath) | Out-Null
    $DownloadPath = "$OfficialArchivePath.download"
    Invoke-WebRequest -Uri $OfficialSourceArchiveUrl -OutFile $DownloadPath
    Move-Item -LiteralPath $DownloadPath -Destination $OfficialArchivePath
}
$ArchiveHash = (Get-FileHash -LiteralPath $OfficialArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $ExpectedSourceArchiveSha256) {
    throw "Official RNNoise source archive hash mismatch: $OfficialArchivePath"
}

# --- 2. Extract the verified source -----------------------------------------
if (Test-Path -LiteralPath $SourceRoot) {
    $ResolvedSource = [System.IO.Path]::GetFullPath($SourceRoot)
    if (-not $ResolvedSource.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove source directory outside RNNoise build root"
    }
    Remove-Item -LiteralPath $ResolvedSource -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $SourceRoot | Out-Null
tar -xf $OfficialArchivePath -C $SourceRoot --strip-components=1
if ($LASTEXITCODE -ne 0) { throw "Could not extract pinned RNNoise source" }

# Cross-check that the extracted tree really is the pinned release and that the
# baked-in model file is present (no separate model download is used).
foreach ($Required in ($LibrarySources + @("include/rnnoise.h"))) {
    if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot $Required) -PathType Leaf)) {
        throw "Extracted RNNoise source is missing an expected file: $Required"
    }
}
if (Test-Path -LiteralPath (Join-Path $SourceRoot "download_model.sh")) {
    throw "Pinned RNNoise release unexpectedly requires an external model download"
}

# --- 3. Build the library from the verified source (CPU only, no GPL) --------
# A minimal CMake project compiles exactly the audited library source list into
# one static lib. No external dependencies, no examples, no training tools, no
# GPL/nonfree code paths. HAVE_CONFIG_H is intentionally left undefined so the
# portable, autotools-free defaults are used.
#
# Toolchain note (Windows): RNNoise v0.1.1's CELT-derived pitch.c / celt_lpc.c
# use C99 variable-length arrays (e.g. `opus_val16 xx[n]`), which MSVC `cl.exe`
# does NOT implement. The library therefore builds with any C99 compiler
# (gcc/clang) or with the LLVM `clang-cl` toolset on Windows (ABI-compatible
# static .lib that links into the MSVC-built adapter), but NOT with plain
# `cl.exe`. This script auto-selects `clang-cl` when the LLVM toolset is present
# and otherwise lets CMake pick the default compiler; on a pure-MSVC machine the
# build will fail here with a clear VLA error, which is expected. `M_PI` is
# supplied via _USE_MATH_DEFINES for every toolchain.
$SourceList = ($LibrarySources | ForEach-Object { "    `"$_`"" }) -join [Environment]::NewLine
$GeneratedCMake = @"
cmake_minimum_required(VERSION 3.25)
project(rnnoise_audited LANGUAGES C)

# CPU-only static build of the audited RNNoise library sources.
add_library(rnnoise STATIC
$SourceList
)
target_include_directories(rnnoise
    PUBLIC "`${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "`${CMAKE_CURRENT_SOURCE_DIR}/src"
)
# MSVC/Windows CRT does not declare M_PI without this; harmless elsewhere.
target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES)
if(MSVC)
    target_compile_options(rnnoise PRIVATE /utf-8)
    # Keep the third-party C at its own warning level rather than the project's
    # /WX gate (that gate applies to Creator Studio's own adapter code).
    target_compile_definitions(rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS rnnoise ARCHIVE DESTINATION lib)
install(FILES "`${CMAKE_CURRENT_SOURCE_DIR}/include/rnnoise.h" DESTINATION include)
"@
[System.IO.File]::WriteAllText(
    (Join-Path $SourceRoot "CMakeLists.txt"),
    $GeneratedCMake, [System.Text.UTF8Encoding]::new($false))

if (Test-Path -LiteralPath $NativeBuildRoot) {
    Remove-Item -LiteralPath $NativeBuildRoot -Recurse -Force
}
if (Test-Path -LiteralPath $InstallRoot) {
    $ResolvedInstall = [System.IO.Path]::GetFullPath($InstallRoot)
    if (-not $ResolvedInstall.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                         [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace install directory outside RNNoise build root"
    }
    Remove-Item -LiteralPath $ResolvedInstall -Recurse -Force
}

# Prefer the LLVM clang-cl toolset (C99 VLA support) when it is available; the
# resulting static lib is MSVC-ABI compatible and links into the adapter.
$ClangClArgs = @()
$ClangCl = (Get-Command clang-cl.exe -ErrorAction SilentlyContinue)
if (-not $ClangCl) {
    $LlvmClangCl = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
    if (Test-Path -LiteralPath $LlvmClangCl) { $ClangCl = $LlvmClangCl }
}
if ($ClangCl) {
    $ClangClPath = if ($ClangCl -is [string]) { $ClangCl } else { $ClangCl.Source }
    Write-Host "Using clang-cl for the RNNoise C99 sources: $ClangClPath"
    $ClangClArgs = @("-DCMAKE_C_COMPILER=$ClangClPath")
}

& cmake -S $SourceRoot -B $NativeBuildRoot -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=$InstallRoot" @ClangClArgs
if ($LASTEXITCODE -ne 0) { throw "RNNoise configure failed" }
& cmake --build $NativeBuildRoot --config Release --target rnnoise
if ($LASTEXITCODE -ne 0) { throw "RNNoise library build failed" }
& cmake --install $NativeBuildRoot --config Release
if ($LASTEXITCODE -ne 0) { throw "RNNoise install failed" }

$StagedLib = Join-Path $InstallRoot "lib/rnnoise.lib"
$StagedHeader = Join-Path $InstallRoot "include/rnnoise.h"
foreach ($Artifact in @($StagedLib, $StagedHeader)) {
    if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) {
        throw "RNNoise install did not produce the expected artifact: $Artifact"
    }
}

# --- 4. Build evidence -------------------------------------------------------
$Evidence = @(
    "Creator Studio RNNoise build evidence",
    "rnnoise_version=$ExpectedRnnoiseVersion",
    "source_commit=$ExpectedSourceCommit",
    "official_source_archive_sha256=$ExpectedSourceArchiveSha256",
    "verified_official_archive=$OfficialArchivePath",
    "license=$ExpectedLicense",
    "linking=static",
    "model_weights=in-source (src/rnn_data.c); no separate model download",
    "gpl=false",
    "nonfree=false",
    "cpu_only=true",
    "",
    "library_sources=" + ($LibrarySources -join ",")
) -join [Environment]::NewLine
$EvidencePath = Join-Path $InstallRoot "creator-studio-rnnoise-build.txt"
[System.IO.File]::WriteAllText($EvidencePath, $Evidence, [System.Text.UTF8Encoding]::new($false))

# --- 5. Runtime manifest -----------------------------------------------------
function Get-CompatibleRelativePath {
    param([string]$BasePath, [string]$FullPath)
    $Base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    $BaseUri = [System.Uri]::new($Base)
    $FullUri = [System.Uri]::new([System.IO.Path]::GetFullPath($FullPath))
    return [System.Uri]::UnescapeDataString(
        $BaseUri.MakeRelativeUri($FullUri).ToString()).Replace(
            '/', [System.IO.Path]::DirectorySeparatorChar)
}

function Get-FileProvenance {
    param([string]$Relative)
    $Lower = $Relative.ToLowerInvariant()
    if ($Lower -eq "creator-studio-rnnoise-build.txt") {
        return [ordered]@{ component = "Creator Studio"; version = "1"; source_identity = "repository:R2-audio-dsp"; license = "LicenseRef-Creator-Studio-Proprietary" }
    }
    if ($Lower -eq "lib/rnnoise.lib" -or $Lower -eq "include/rnnoise.h") {
        return [ordered]@{ component = "RNNoise"; version = $ExpectedRnnoiseVersion; source_identity = $ExpectedSourceCommit; license = $ExpectedLicense }
    }
    throw "No approved provenance classification for RNNoise artifact: $Relative"
}

$ManifestFiles = @()
foreach ($File in Get-ChildItem -LiteralPath $InstallRoot -Recurse -File | Sort-Object FullName) {
    if ($File.Name -eq "rnnoise-runtime-manifest.json") { continue }
    $Relative = (Get-CompatibleRelativePath $InstallRoot $File.FullName).Replace('\', '/')
    $Role = if ($Relative -eq "creator-studio-rnnoise-build.txt") { "evidence" } else { "development" }
    $Provenance = Get-FileProvenance $Relative
    $ManifestFiles += [ordered]@{
        path = $Relative
        sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        role = $Role
        component = $Provenance.component
        version = $Provenance.version
        source_identity = $Provenance.source_identity
        license = $Provenance.license
    }
}
$Manifest = [ordered]@{
    abi = 1
    component = "RNNoise"
    version = $ExpectedRnnoiseVersion
    source_commit = $ExpectedSourceCommit
    source_archive_sha256 = $ExpectedSourceArchiveSha256
    linking = "static"
    license = $ExpectedLicense
    files = $ManifestFiles
}
$ManifestPath = Join-Path $InstallRoot "rnnoise-runtime-manifest.json"
[System.IO.File]::WriteAllText(
    $ManifestPath, ($Manifest | ConvertTo-Json -Depth 6),
    [System.Text.UTF8Encoding]::new($false))

& (Join-Path $PSScriptRoot "verify_rnnoise_runtime.ps1") -RuntimeRoot $InstallRoot -ManifestPath $ManifestPath
if ($LASTEXITCODE -ne 0) { throw "Generated RNNoise runtime did not pass verification" }

Write-Host "CS_RNNOISE_ROOT=$InstallRoot"
Write-Host "Audited RNNoise root: $InstallRoot"
Write-Host "Build evidence: $EvidencePath"
Write-Host "Runtime manifest: $ManifestPath"
