[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$DestinationRoot
)

$ErrorActionPreference = "Stop"
$SourceRoot = [System.IO.Path]::GetFullPath($SourceRoot)
$DestinationRoot = [System.IO.Path]::GetFullPath($DestinationRoot)
$ManifestName = "mlt-runtime-manifest.json"
$SourceManifestPath = Join-Path $SourceRoot $ManifestName
$AllowedRuntimeRoles = @(
    "runtime-library",
    "runtime-module",
    "runtime-data",
    "evidence"
)

if ([System.IO.Path]::GetFileName($DestinationRoot) -ne "mlt-runtime") {
    throw "MLT staging destination must be a directory named mlt-runtime"
}
if ($SourceRoot -eq $DestinationRoot) {
    throw "MLT source and staging destination must differ"
}

& (Join-Path $PSScriptRoot "verify_mlt_runtime.ps1") `
    -RuntimeRoot $SourceRoot -ManifestPath $SourceManifestPath

$SourceManifest = Get-Content -LiteralPath $SourceManifestPath `
    -Raw -Encoding utf8 | ConvertFrom-Json
$RuntimeFiles = @()
foreach ($Entry in $SourceManifest.files) {
    $Role = [string]$Entry.role
    if ($Role -eq "development") { continue }
    if ($AllowedRuntimeRoles -notcontains $Role) {
        throw "Unsupported MLT manifest role while staging: $Role"
    }
    $RuntimeFiles += $Entry
}
if ($RuntimeFiles.Count -eq 0) {
    throw "MLT manifest contains no runtime files"
}

$Parent = [System.IO.Directory]::GetParent($DestinationRoot).FullName
[System.IO.Directory]::CreateDirectory($Parent) | Out-Null
$TemporaryRoot = Join-Path $Parent `
    (".mlt-runtime.stage-" + [System.Guid]::NewGuid().ToString("N"))

try {
    [System.IO.Directory]::CreateDirectory($TemporaryRoot) | Out-Null
    foreach ($Entry in $RuntimeFiles) {
        $Relative = ([string]$Entry.path).Replace(
            '/', [System.IO.Path]::DirectorySeparatorChar)
        $Source = Join-Path $SourceRoot $Relative
        $Destination = Join-Path $TemporaryRoot $Relative
        [System.IO.Directory]::CreateDirectory(
            [System.IO.Path]::GetDirectoryName($Destination)) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination
    }

    $StagedManifest = [ordered]@{
        abi = $SourceManifest.abi
        component = $SourceManifest.component
        version = $SourceManifest.version
        source_commit = $SourceManifest.source_commit
        linking = $SourceManifest.linking
        allowed_modules = @($SourceManifest.allowed_modules)
        dependencies = @($SourceManifest.dependencies)
        files = @($RuntimeFiles)
    }
    $TemporaryManifestPath = Join-Path $TemporaryRoot $ManifestName
    [System.IO.File]::WriteAllText(
        $TemporaryManifestPath,
        ($StagedManifest | ConvertTo-Json -Depth 6),
        [System.Text.UTF8Encoding]::new($false))

    & (Join-Path $PSScriptRoot "verify_mlt_runtime.ps1") `
        -RuntimeRoot $TemporaryRoot -ManifestPath $TemporaryManifestPath

    if (Test-Path -LiteralPath $DestinationRoot) {
        Remove-Item -LiteralPath $DestinationRoot -Recurse -Force
    }
    Move-Item -LiteralPath $TemporaryRoot -Destination $DestinationRoot
} finally {
    if (Test-Path -LiteralPath $TemporaryRoot) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
}

Write-Host "Staged audited runtime-only MLT package: $DestinationRoot"
