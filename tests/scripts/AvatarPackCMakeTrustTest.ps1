[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$RuntimeRoot = "",
    [Parameter(Mandatory = $true)]
    [string]$Qt6Dir,
    [Parameter(Mandatory = $true)]
    [string]$MinizSourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$JsonSourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$JsonValidatorSourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$SqliteSourceRoot,
    [string]$ScratchRoot = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    $RuntimeRoot = Join-Path $RepositoryRoot "build/sodium/prefix"
}
if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $ScratchRoot = Join-Path $RepositoryRoot "build/avatar-pack-cmake-trust-test"
}
$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
$ScratchRoot = [System.IO.Path]::GetFullPath($ScratchRoot)
$Qt6Dir = [System.IO.Path]::GetFullPath($Qt6Dir)
$MinizSourceRoot = [System.IO.Path]::GetFullPath($MinizSourceRoot)
$JsonSourceRoot = [System.IO.Path]::GetFullPath($JsonSourceRoot)
$JsonValidatorSourceRoot =
    [System.IO.Path]::GetFullPath($JsonValidatorSourceRoot)
$SqliteSourceRoot = [System.IO.Path]::GetFullPath($SqliteSourceRoot)
$BuildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $RepositoryRoot "build")
).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $ScratchRoot.StartsWith(
    $BuildRoot,
    [System.StringComparison]::OrdinalIgnoreCase
)) {
    throw "ScratchRoot must remain inside the repository build directory"
}

foreach ($RequiredPath in @(
    (Join-Path $RuntimeRoot "include/sodium.h"),
    (Join-Path $RuntimeRoot "lib/libsodium.lib"),
    (Join-Path $RuntimeRoot "bin/libsodium.dll"),
    (Join-Path $Qt6Dir "Qt6Config.cmake"),
    (Join-Path $MinizSourceRoot "miniz.c"),
    (Join-Path $MinizSourceRoot "miniz.h"),
    (Join-Path $JsonSourceRoot "include/nlohmann/json.hpp"),
    (Join-Path $JsonValidatorSourceRoot "CMakeLists.txt"),
    (Join-Path $SqliteSourceRoot "sqlite3.c")
)) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "CMake trust test input is missing: $RequiredPath"
    }
}
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw "cmake.exe is required for the CMake trust regression"
}
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    throw "ninja.exe is required for the CMake trust regression"
}

if (Test-Path -LiteralPath $ScratchRoot) {
    Remove-Item -LiteralPath $ScratchRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null

$RootA = Join-Path $ScratchRoot "sodium-root-a"
$RootB = Join-Path $ScratchRoot "sodium-root-b"
Copy-Item -LiteralPath $RuntimeRoot -Destination $RootA -Recurse
Copy-Item -LiteralPath $RuntimeRoot -Destination $RootB -Recurse
$OutsideRoot = Join-Path $ScratchRoot "outside"
New-Item -ItemType Directory -Force -Path $OutsideRoot | Out-Null
$OutsideLibrary = Join-Path $OutsideRoot "libsodium.lib"
Copy-Item `
    -LiteralPath (Join-Path $RuntimeRoot "lib/libsodium.lib") `
    -Destination $OutsideLibrary

$ConfigureRoot = Join-Path $ScratchRoot "configure"
$Problems = @()

function Invoke-Configure {
    param(
        [string]$Name,
        [string]$SodiumRoot,
        [string[]]$AdditionalArguments
    )
    $Arguments = @(
        "-S", $RepositoryRoot,
        "-B", $ConfigureRoot,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCS_BUILD_TESTS=OFF",
        "-DCS_WARNINGS_AS_ERRORS=ON",
        "-DCS_ENABLE_AVATAR_PACKS=ON",
        "-DCS_SODIUM_ROOT=$SodiumRoot",
        "-DQt6_DIR=$Qt6Dir",
        "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$JsonSourceRoot",
        "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON_SCHEMA_VALIDATOR=$JsonValidatorSourceRoot",
        "-DFETCHCONTENT_SOURCE_DIR_SQLITE_AMALGAMATION=$SqliteSourceRoot"
    ) + $AdditionalArguments
    $LogPath = Join-Path $ScratchRoot "$Name.log"
    $SavedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & cmake.exe @Arguments *> $LogPath
    $ExitCode = $LASTEXITCODE
    $ErrorActionPreference = $SavedPreference
    return [pscustomobject]@{
        exit_code = $ExitCode
        log_path = $LogPath
        output = Get-Content -LiteralPath $LogPath -Raw
    }
}

function Convert-CMakePath {
    param([string]$Path)
    return [System.IO.Path]::GetFullPath($Path).Replace('\', '/')
}

function Assert-ResolvedRoot {
    param(
        [object]$Result,
        [string]$ExpectedRoot,
        [string]$Context
    )
    $ExpectedLibrary = Convert-CMakePath `
        -Path (Join-Path $ExpectedRoot "lib/libsodium.lib")
    $ExpectedRuntime = Convert-CMakePath `
        -Path (Join-Path $ExpectedRoot "bin/libsodium.dll")
    $ExpectedInclude = Convert-CMakePath `
        -Path (Join-Path $ExpectedRoot "include")
    foreach ($ExpectedLine in @(
        "Sodium::Sodium include directory: $ExpectedInclude",
        "Sodium::Sodium import library: $ExpectedLibrary",
        "Sodium::Sodium runtime library: $ExpectedRuntime"
    )) {
        if ($Result.output -notmatch [regex]::Escape($ExpectedLine)) {
            $script:Problems += "$Context did not resolve the imported target to: $ExpectedLine"
        }
    }
}

$CacheEscape = Invoke-Configure `
    -Name "cache-escape" `
    -SodiumRoot $RootA `
    -AdditionalArguments @("-DSodium_LIBRARY=$OutsideLibrary")
if ($CacheEscape.exit_code -ne 0) {
    $Problems += "Cache-escape baseline configure failed unexpectedly"
} else {
    Assert-ResolvedRoot `
        -Result $CacheEscape `
        -ExpectedRoot $RootA `
        -Context "Cache-escape configure"
    $CacheText = Get-Content -LiteralPath (Join-Path $ConfigureRoot "CMakeCache.txt") -Raw
    if ($CacheText -match [regex]::Escape((Convert-CMakePath -Path $OutsideLibrary))) {
        $Problems += "Sodium_LIBRARY command-line cache escaped CS_SODIUM_ROOT"
    }
}

$RootChange = Invoke-Configure `
    -Name "root-change" `
    -SodiumRoot $RootB `
    -AdditionalArguments @("-USodium_LIBRARY")
if ($RootChange.exit_code -ne 0) {
    $Problems += "Root-change configure failed unexpectedly"
} else {
    Assert-ResolvedRoot `
        -Result $RootChange `
        -ExpectedRoot $RootB `
        -Context "Root-change configure"
    $CacheText = Get-Content -LiteralPath (Join-Path $ConfigureRoot "CMakeCache.txt") -Raw
    if ($CacheText -match [regex]::Escape((Convert-CMakePath -Path $RootA))) {
        $Problems += "Stale Sodium cache paths survived CS_SODIUM_ROOT change"
    }
}

$PopulatedMinizHeader = Join-Path $ConfigureRoot "_deps/miniz-src/miniz.h"
if (-not (Test-Path -LiteralPath $PopulatedMinizHeader -PathType Leaf)) {
    $Problems += "Configured miniz source header is missing before mutation regression"
} else {
    [System.IO.File]::AppendAllText($PopulatedMinizHeader, "tampered-after-configure")
    $PostConfigureMutation = Invoke-Configure `
        -Name "miniz-post-configure-mutation" `
        -SodiumRoot $RootB `
        -AdditionalArguments @()
    if ($PostConfigureMutation.exit_code -eq 0) {
        $Problems += "Post-configure miniz source mutation was accepted"
    } elseif ($PostConfigureMutation.output -notmatch
        "miniz source tree does not match the pinned official 3\.1\.2 archive") {
        $Problems += "Post-configure miniz mutation failed outside canonical source verification"
    }
}

$SourceOverride = Invoke-Configure `
    -Name "miniz-source-override" `
    -SodiumRoot $RootB `
    -AdditionalArguments @("-DFETCHCONTENT_SOURCE_DIR_MINIZ=$MinizSourceRoot")
if ($SourceOverride.exit_code -eq 0) {
    $Problems += "FETCHCONTENT_SOURCE_DIR_MINIZ bypass was accepted"
} elseif ($SourceOverride.output -notmatch
    "Audited miniz forbids dependency override: FETCHCONTENT_SOURCE_DIR_MINIZ") {
    $Problems += "FETCHCONTENT_SOURCE_DIR_MINIZ failed outside the explicit override guard"
}

$Disconnected = Invoke-Configure `
    -Name "miniz-disconnected" `
    -SodiumRoot $RootB `
    -AdditionalArguments @(
        "-UFETCHCONTENT_SOURCE_DIR_MINIZ",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    )
if ($Disconnected.exit_code -eq 0) {
    $Problems += "FETCHCONTENT_FULLY_DISCONNECTED bypass was accepted"
} elseif ($Disconnected.output -notmatch
    "Audited miniz forbids FETCHCONTENT_FULLY_DISCONNECTED") {
    $Problems += "FETCHCONTENT_FULLY_DISCONNECTED failed outside the explicit guard"
}

if ($Problems.Count -gt 0) {
    throw "Avatar pack CMake trust regressions failed: $($Problems -join '; ')"
}

Write-Host "Avatar pack CMake dependency trust boundaries are fail-closed."
