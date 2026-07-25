[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$RuntimeRoot = "",
    [Parameter(Mandatory = $true)]
    [string]$Qt6Dir,
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
    $ScratchRoot = Join-Path $RepositoryRoot "build/avatar-pack-post-configure-audit-test"
}
$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
$ScratchRoot = [System.IO.Path]::GetFullPath($ScratchRoot)
$Qt6Dir = [System.IO.Path]::GetFullPath($Qt6Dir)
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
    (Join-Path $RuntimeRoot "runtime-manifest.json"),
    (Join-Path $RuntimeRoot "lib/libsodium.lib"),
    (Join-Path $RuntimeRoot "bin/libsodium.dll"),
    (Join-Path $Qt6Dir "Qt6Config.cmake"),
    (Join-Path $JsonSourceRoot "include/nlohmann/json.hpp"),
    (Join-Path $JsonValidatorSourceRoot "CMakeLists.txt"),
    (Join-Path $SqliteSourceRoot "sqlite3.c")
)) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "Build audit test input is missing: $RequiredPath"
    }
}
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw "cmake.exe is required for the post-configure audit regression"
}
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    throw "ninja.exe is required for the post-configure audit regression"
}

if (Test-Path -LiteralPath $ScratchRoot) {
    Remove-Item -LiteralPath $ScratchRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null

function Invoke-NativeLogged {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$LogPath
    )
    $SavedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Executable @Arguments *> $LogPath
    $ExitCode = $LASTEXITCODE
    $ErrorActionPreference = $SavedPreference
    return [pscustomobject]@{
        exit_code = $ExitCode
        output = Get-Content -LiteralPath $LogPath -Raw
    }
}

$Cases = @(
    [pscustomobject]@{
        name = "miniz-c"
        relative_path = "_deps/miniz-src/miniz.c"
        expected_error = "miniz source tree does not match the pinned official 3.1.2 archive"
        mutation_bytes = [System.Text.Encoding]::ASCII.GetBytes(
            "`r`n/* task4 post-configure mutation */`r`n"
        )
    },
    [pscustomobject]@{
        name = "libsodium-lib"
        relative_path = "sodium-prefix/lib/libsodium.lib"
        expected_error = "pinned trust anchor"
        mutation_bytes = [byte[]]@(0x41)
    },
    [pscustomobject]@{
        name = "libsodium-dll"
        relative_path = "sodium-prefix/bin/libsodium.dll"
        expected_error = "pinned trust anchor"
        mutation_bytes = [byte[]]@(0x41)
    }
)
$AcceptedCases = @()
$WrongFailureCases = @()

foreach ($Case in $Cases) {
    $CaseRoot = Join-Path $ScratchRoot $Case.name
    $CaseRuntime = Join-Path $CaseRoot "sodium-prefix"
    $CaseBuild = Join-Path $CaseRoot "build"
    New-Item -ItemType Directory -Force -Path $CaseRoot | Out-Null
    Copy-Item -LiteralPath $RuntimeRoot -Destination $CaseRuntime -Recurse

    $Configure = Invoke-NativeLogged `
        -Executable "cmake.exe" `
        -Arguments @(
            "-S", $RepositoryRoot,
            "-B", $CaseBuild,
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCS_BUILD_TESTS=OFF",
            "-DCS_WARNINGS_AS_ERRORS=ON",
            "-DCS_ENABLE_AVATAR_PACKS=ON",
            "-DCS_SODIUM_ROOT=$CaseRuntime",
            "-DQt6_DIR=$Qt6Dir",
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$JsonSourceRoot",
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON_SCHEMA_VALIDATOR=$JsonValidatorSourceRoot",
            "-DFETCHCONTENT_SOURCE_DIR_SQLITE_AMALGAMATION=$SqliteSourceRoot"
        ) `
        -LogPath (Join-Path $CaseRoot "configure.log")
    if ($Configure.exit_code -ne 0) {
        throw "Baseline configure failed for $($Case.name): $($Configure.output)"
    }

    $MutationPath = if ($Case.name -eq "miniz-c") {
        Join-Path $CaseBuild $Case.relative_path
    } else {
        Join-Path $CaseRoot $Case.relative_path
    }
    if (-not (Test-Path -LiteralPath $MutationPath -PathType Leaf)) {
        throw "Mutation target is missing for $($Case.name): $MutationPath"
    }
    $OriginalBytes = [System.IO.File]::ReadAllBytes($MutationPath)
    try {
        $TamperedBytes = New-Object byte[] (
            $OriginalBytes.Length + $Case.mutation_bytes.Length
        )
        [System.Array]::Copy($OriginalBytes, $TamperedBytes, $OriginalBytes.Length)
        [System.Array]::Copy(
            $Case.mutation_bytes,
            0,
            $TamperedBytes,
            $OriginalBytes.Length,
            $Case.mutation_bytes.Length
        )
        [System.IO.File]::WriteAllBytes($MutationPath, $TamperedBytes)

        $Build = Invoke-NativeLogged `
            -Executable "cmake.exe" `
            -Arguments @(
                "--build", $CaseBuild,
                "--target", "cs_avatar_pack_adapter"
            ) `
            -LogPath (Join-Path $CaseRoot "build-after-mutation.log")
        if ($Build.exit_code -eq 0) {
            $AcceptedCases += $Case.name
        } elseif ($Build.output -notmatch [regex]::Escape($Case.expected_error)) {
            $WrongFailureCases += $Case.name
        }
    } finally {
        [System.IO.File]::WriteAllBytes($MutationPath, $OriginalBytes)
    }
}

if ($AcceptedCases.Count -gt 0) {
    throw "Post-configure dependency mutations built successfully: $($AcceptedCases -join '; ')"
}
if ($WrongFailureCases.Count -gt 0) {
    throw "Post-configure mutations failed outside the dependency audit: $($WrongFailureCases -join '; ')"
}

Write-Host "Every adapter build re-audits miniz and libsodium content."
