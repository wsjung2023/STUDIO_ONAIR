[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"
$ExpectedVersion = "7.40.0"
$ExpectedSourceCommit = "bef9d89c0c279e558d9625dac3399c2aa3d961bc"
$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $RuntimeRoot "mlt-runtime-manifest.json"
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
    throw "MLT runtime manifest is missing: $ManifestPath"
}
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($Manifest.version -ne $ExpectedVersion) {
    throw "MLT runtime version mismatch: expected $ExpectedVersion"
}
if ($Manifest.source_commit -ne $ExpectedSourceCommit) {
    throw "MLT runtime source_commit mismatch"
}
if ($Manifest.abi -ne 1) {
    throw "Unsupported MLT runtime manifest ABI"
}

$Expected = @{}
foreach ($Entry in $Manifest.files) {
    $Relative = [string]$Entry.path
    if ([string]::IsNullOrWhiteSpace($Relative) -or
        [System.IO.Path]::IsPathRooted($Relative) -or
        $Relative -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "Invalid path in MLT runtime manifest"
    }
    $Normalized = $Relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    $FullPath = [System.IO.Path]::GetFullPath((Join-Path $RuntimeRoot $Normalized))
    if (-not $FullPath.StartsWith($RuntimeRoot + [System.IO.Path]::DirectorySeparatorChar,
                                  [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes MLT runtime root"
    }
    if ($Expected.ContainsKey($Relative)) {
        throw "Duplicate path in MLT runtime manifest: $Relative"
    }
    if ($Relative -match '(?i)(^|/)(melt(?:\.exe)?|.*plusgpl.*|.*rubberband.*|.*vid\.stab.*|.*xine.*)$') {
        throw "forbidden MLT artifact in manifest: $Relative"
    }
    $Expected[$Relative] = [string]$Entry.sha256
}

$Actual = @{}
foreach ($File in Get-ChildItem -LiteralPath $RuntimeRoot -Recurse -File) {
    if ($File.FullName -eq $ManifestPath) { continue }
    if (($File.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Reparse points are forbidden in the MLT runtime: $($File.Name)"
    }
    $Relative = (Get-CompatibleRelativePath $RuntimeRoot $File.FullName).Replace('\', '/')
    if ($Relative -match '(?i)(^|/)(melt(?:\.exe)?|.*plusgpl.*|.*rubberband.*|.*vid\.stab.*|.*xine.*)$') {
        throw "forbidden MLT runtime artifact: $Relative"
    }
    $Actual[$Relative] = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}

foreach ($Relative in $Expected.Keys) {
    if (-not $Actual.ContainsKey($Relative)) {
        throw "Missing MLT runtime artifact: $Relative"
    }
    if ($Actual[$Relative] -ne $Expected[$Relative]) {
        throw "SHA256 mismatch for MLT runtime artifact: $Relative"
    }
}
foreach ($Relative in $Actual.Keys) {
    if (-not $Expected.ContainsKey($Relative)) {
        throw "unexpected MLT runtime artifact: $Relative"
    }
}

Write-Host "Verified audited MLT $ExpectedVersion runtime: $RuntimeRoot"
