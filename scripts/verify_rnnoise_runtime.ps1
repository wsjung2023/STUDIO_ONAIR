# Fail-closed verification of an audited RNNoise prefix.
#
# Mirrors scripts/verify_mlt_runtime.ps1: parse the runtime manifest, confirm
# it describes the pinned RNNoise release, then prove the on-disk prefix is
# EXACTLY that manifest — every listed file present with the recorded SHA-256,
# no unexpected files, no path traversal, no reparse points, no forbidden or
# GPL artifacts. CMake runs this at configure time (CS_ENABLE_RNNOISE) and the
# bootstrap runs it after staging, so a tampered or drifted prefix is rejected
# before the denoiser links against it.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"
$ExpectedVersion = "0.1.1"
$ExpectedSourceCommit = "6cbfd53eb348a8d394e0757b4025c6ded34eb2b6"
$ExpectedSourceArchiveSha256 = "1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987"
$ExpectedLicense = "BSD-3-Clause"
$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $RuntimeRoot "rnnoise-runtime-manifest.json"
}
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)

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

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "RNNoise runtime manifest is missing: $ManifestPath"
}
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($Manifest.abi -ne 1) {
    throw "Unsupported RNNoise runtime manifest ABI"
}
if ($Manifest.component -ne "RNNoise") {
    throw "RNNoise runtime component mismatch"
}
if ($Manifest.version -ne $ExpectedVersion) {
    throw "RNNoise runtime version mismatch: expected $ExpectedVersion"
}
if ($Manifest.source_commit -ne $ExpectedSourceCommit) {
    throw "RNNoise runtime source_commit mismatch"
}
if ($Manifest.source_archive_sha256 -ne $ExpectedSourceArchiveSha256) {
    throw "RNNoise runtime source_archive_sha256 mismatch"
}
if ($Manifest.linking -ne "static") {
    throw "RNNoise runtime linking mismatch"
}
if ($Manifest.license -ne $ExpectedLicense) {
    throw "RNNoise runtime license mismatch"
}

function Get-ApprovedProvenance {
    param([string]$Relative)
    $Lower = $Relative.ToLowerInvariant()
    if ($Lower -eq "creator-studio-rnnoise-build.txt") {
        return @("Creator Studio", "1", "repository:R2-audio-dsp", "LicenseRef-Creator-Studio-Proprietary")
    }
    if ($Lower -eq "lib/rnnoise.lib" -or $Lower -eq "include/rnnoise.h") {
        return @("RNNoise", $ExpectedVersion, $ExpectedSourceCommit, $ExpectedLicense)
    }
    throw "No approved provenance classification for RNNoise artifact: $Relative"
}

$AllowedRoles = @("development", "evidence")
$Expected = @{}
foreach ($Entry in $Manifest.files) {
    $Relative = [string]$Entry.path
    if ([string]::IsNullOrWhiteSpace($Relative) -or
        $Relative.Contains('\') -or
        [System.IO.Path]::IsPathRooted($Relative) -or
        $Relative -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "Invalid path in RNNoise runtime manifest: $Relative"
    }
    $Normalized = $Relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    $FullPath = [System.IO.Path]::GetFullPath((Join-Path $RuntimeRoot $Normalized))
    if (-not $FullPath.StartsWith($RuntimeRoot + [System.IO.Path]::DirectorySeparatorChar,
                                  [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes RNNoise runtime root: $Relative"
    }
    if ($Expected.ContainsKey($Relative)) {
        throw "Duplicate path in RNNoise runtime manifest: $Relative"
    }
    if ($Relative -match '(?i)\.(exe|dll|so|dylib)$' -or
        $Relative -match '(?i)gpl') {
        throw "forbidden RNNoise artifact in manifest: $Relative"
    }
    if ($AllowedRoles -notcontains [string]$Entry.role) {
        throw "Unapproved role in RNNoise runtime manifest: $($Entry.role)"
    }
    if ([string]$Entry.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "Invalid SHA-256 in RNNoise runtime manifest: $Relative"
    }
    $Approved = Get-ApprovedProvenance $Relative
    if ($Entry.component -ne $Approved[0] -or $Entry.version -ne $Approved[1] -or
        $Entry.source_identity -ne $Approved[2] -or $Entry.license -ne $Approved[3]) {
        throw "Unapproved file provenance in RNNoise runtime manifest: $Relative"
    }
    $Expected[$Relative] = [string]$Entry.sha256.ToLowerInvariant()
}

foreach ($Required in @("lib/rnnoise.lib", "include/rnnoise.h")) {
    if (-not $Expected.ContainsKey($Required)) {
        throw "RNNoise runtime is missing a required approved component: $Required"
    }
}

$Actual = @{}
foreach ($File in Get-ChildItem -LiteralPath $RuntimeRoot -Recurse -File) {
    if ($File.FullName -eq $ManifestPath) { continue }
    if (($File.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Reparse points are forbidden in the RNNoise runtime: $($File.Name)"
    }
    $Relative = (Get-CompatibleRelativePath $RuntimeRoot $File.FullName).Replace('\', '/')
    if ($Relative -match '(?i)\.(exe|dll|so|dylib)$' -or $Relative -match '(?i)gpl') {
        throw "forbidden RNNoise runtime artifact: $Relative"
    }
    $Actual[$Relative] = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}

foreach ($Relative in $Expected.Keys) {
    if (-not $Actual.ContainsKey($Relative)) {
        throw "Missing RNNoise runtime artifact: $Relative"
    }
    if ($Actual[$Relative] -ne $Expected[$Relative]) {
        throw "SHA256 mismatch for RNNoise runtime artifact: $Relative"
    }
}
foreach ($Relative in $Actual.Keys) {
    if (-not $Expected.ContainsKey($Relative)) {
        throw "unexpected RNNoise runtime artifact: $Relative"
    }
}

Write-Host "Verified audited RNNoise $ExpectedVersion runtime: $RuntimeRoot"
