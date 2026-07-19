# Creator Studio build + verify wrapper.
#
# Encodes CLAUDE.md §10 (build -> test) plus the Qt-free boundary guard so any
# agent (or human) can prove a change is green with one command instead of
# re-deriving the MSVC + Qt + Ninja environment each session.
#
#   scripts/studio-build-verify.ps1                 # windows-debug, full ctest
#   scripts/studio-build-verify.ps1 -Ffmpeg         # windows-ffmpeg-debug variant
#   scripts/studio-build-verify.ps1 -Preset windows-release
#   scripts/studio-build-verify.ps1 -SkipTests      # build only (fast qt-free check)
#
# The Qt-free guard (`cs_assert_qt_free`, cmake/CreatorStudioTargets.cmake) is a
# compile-time include barrier: it runs automatically during the build step, so a
# successful build already proves the Qt-free modules link no Qt. No separate
# command is needed.
[CmdletBinding()]
param(
    [ValidateSet("windows-debug", "windows-release",
                 "windows-ffmpeg-debug", "windows-ffmpeg-release",
                 "windows-rnnoise-debug", "windows-rnnoise-release",
                 "windows-whisper-debug", "windows-whisper-release")]
    [string]$Preset = "windows-debug",
    [switch]$Ffmpeg,
    [switch]$Rnnoise,
    [switch]$Whisper,
    [switch]$SkipTests,
    [switch]$Fresh,
    [string]$VsRoot = "",
    [string]$QtPrefix = "C:\Qt\6.8.3\msvc2022_64"
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# -Ffmpeg is sugar for the audited-FFmpeg preset family (CS_ENABLE_FFMPEG ON).
if ($Ffmpeg -and ($Preset -notlike "*ffmpeg*")) {
    $Preset = $Preset -replace "^windows-", "windows-ffmpeg-"
}
# -Rnnoise is sugar for the audited-RNNoise preset family (CS_ENABLE_RNNOISE ON).
if ($Rnnoise -and ($Preset -notlike "*rnnoise*")) {
    $Preset = $Preset -replace "^windows-", "windows-rnnoise-"
}
# -Whisper is sugar for the audited whisper.cpp preset family (CS_ENABLE_WHISPER ON).
if ($Whisper -and ($Preset -notlike "*whisper*")) {
    $Preset = $Preset -replace "^windows-", "windows-whisper-"
}

function Resolve-VsRoot {
    param([string]$Explicit)
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) { return $Explicit }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($found) { return ($found | Select-Object -First 1) }
    }
    return "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
}

$VsRoot = Resolve-VsRoot -Explicit $VsRoot
$vcvars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found under '$VsRoot'. Pass -VsRoot <VS install path>."
}
if (-not (Test-Path -LiteralPath $QtPrefix)) {
    throw "Qt prefix '$QtPrefix' not found. Pass -QtPrefix <Qt msvc kit path>."
}

Write-Host "==> Importing MSVC x64 environment (vcvars64)" -ForegroundColor Cyan
# Import the developer environment into this process so cmake/ninja/cl resolve.
cmd /c "call `"$vcvars`" > nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

# VS ships CMake + Ninja but does not always add them to PATH after vcvars.
$cmakeBin = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaBin = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
foreach ($dir in @($cmakeBin, $ninjaBin)) {
    if ((Test-Path -LiteralPath $dir) -and ($env:PATH -notlike "*$dir*")) {
        $env:PATH = "$env:PATH;$dir"
    }
}
$env:CMAKE_PREFIX_PATH = if ([string]::IsNullOrWhiteSpace($env:CMAKE_PREFIX_PATH)) {
    $QtPrefix
} else {
    "$QtPrefix;$env:CMAKE_PREFIX_PATH"
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not on PATH after environment setup (looked under '$cmakeBin')."
}

Push-Location $RepositoryRoot
try {
    if ($Fresh) {
        $buildDir = Join-Path $RepositoryRoot "build/$Preset"
        if (Test-Path -LiteralPath $buildDir) {
            Write-Host "==> -Fresh: removing $buildDir" -ForegroundColor Yellow
            Remove-Item -Recurse -Force -LiteralPath $buildDir
        }
    }

    Write-Host "==> Configure  (preset: $Preset)" -ForegroundColor Cyan
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

    Write-Host "==> Build      (preset: $Preset)  [Qt-free guard runs here]" -ForegroundColor Cyan
    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

    if ($SkipTests) {
        Write-Host "==> Tests skipped (-SkipTests)" -ForegroundColor Yellow
    } else {
        Write-Host "==> Test       (preset: $Preset)" -ForegroundColor Cyan
        ctest --preset $Preset --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "ctest failed ($LASTEXITCODE)" }
    }

    Write-Host ""
    Write-Host "GREEN: configure + build$(if(-not $SkipTests){' + full ctest'}) passed for '$Preset'." -ForegroundColor Green
    Write-Host "       Qt-free boundary held (build-time cs_assert_qt_free)." -ForegroundColor Green
}
finally {
    Pop-Location
}
