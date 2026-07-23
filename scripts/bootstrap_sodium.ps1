[CmdletBinding()]
param(
    [string]$BuildRoot = "",
    [string]$InstallRoot = "",
    [string]$OfficialArchivePath = ""
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$ExpectedVersion = "1.0.22"
$OfficialSourceUrl = "https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-msvc.zip"
$ExpectedArchiveSha256 = "3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279"

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepositoryRoot "build/sodium"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $BuildRoot "prefix"
}
if ([string]::IsNullOrWhiteSpace($OfficialArchivePath)) {
    $OfficialArchivePath = Join-Path $RepositoryRoot "build/downloads/libsodium-1.0.22-msvc.zip"
}

$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$OfficialArchivePath = [System.IO.Path]::GetFullPath($OfficialArchivePath)
$ExtractRoot = Join-Path $BuildRoot "archive"

function Assert-ChildPath {
    param(
        [string]$Path,
        [string]$Parent,
        [string]$Description
    )
    $ResolvedPath = [System.IO.Path]::GetFullPath($Path)
    $ResolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $ResolvedPath.StartsWith(
        $ResolvedParent,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "$Description must remain inside the libsodium build root"
    }
}

function Get-CompatibleRelativePath {
    param([string]$BasePath, [string]$FullPath)
    $Base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    $BaseUri = New-Object System.Uri($Base)
    $FullUri = New-Object System.Uri([System.IO.Path]::GetFullPath($FullPath))
    return [System.Uri]::UnescapeDataString(
        $BaseUri.MakeRelativeUri($FullUri).ToString()
    ).Replace('\', '/')
}

Assert-ChildPath -Path $InstallRoot -Parent $BuildRoot -Description "InstallRoot"
Assert-ChildPath -Path $ExtractRoot -Parent $BuildRoot -Description "ExtractRoot"

if (-not (Test-Path -LiteralPath $OfficialArchivePath -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OfficialArchivePath) |
        Out-Null
    $DownloadPath = "$OfficialArchivePath.download"
    Invoke-WebRequest -UseBasicParsing -Uri $OfficialSourceUrl -OutFile $DownloadPath
    $DownloadHash = (Get-FileHash -LiteralPath $DownloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($DownloadHash -ne $ExpectedArchiveSha256) {
        Remove-Item -LiteralPath $DownloadPath -Force
        throw "Downloaded libsodium archive hash mismatch"
    }
    Move-Item -LiteralPath $DownloadPath -Destination $OfficialArchivePath
}

$ArchiveHash = (Get-FileHash -LiteralPath $OfficialArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $ExpectedArchiveSha256) {
    throw "Official libsodium archive hash mismatch: $OfficialArchivePath"
}
Write-Host "libsodium archive SHA-256: $ArchiveHash"

foreach ($Directory in @($ExtractRoot, $InstallRoot)) {
    if (Test-Path -LiteralPath $Directory) {
        Assert-ChildPath -Path $Directory -Parent $BuildRoot -Description "Removal target"
        Remove-Item -LiteralPath $Directory -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $ExtractRoot | Out-Null
Expand-Archive -LiteralPath $OfficialArchivePath -DestinationPath $ExtractRoot

$PackageRoot = Join-Path $ExtractRoot "libsodium"
$SourceInclude = Join-Path $PackageRoot "include"
$SourceLibrary = Join-Path $PackageRoot "x64/Release/v143/dynamic/libsodium.lib"
$SourceDll = Join-Path $PackageRoot "x64/Release/v143/dynamic/libsodium.dll"
foreach ($RequiredPath in @(
    (Join-Path $SourceInclude "sodium.h"),
    $SourceLibrary,
    $SourceDll
)) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "Verified libsodium archive is missing an expected MSVC artifact: $RequiredPath"
    }
}

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
Copy-Item -LiteralPath $SourceInclude -Destination $InstallRoot -Recurse
New-Item -ItemType Directory -Force -Path (Join-Path $InstallRoot "lib") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $InstallRoot "bin") | Out-Null
Copy-Item -LiteralPath $SourceLibrary -Destination (Join-Path $InstallRoot "lib/libsodium.lib")
Copy-Item -LiteralPath $SourceDll -Destination (Join-Path $InstallRoot "bin/libsodium.dll")

$ManifestFiles = @()
foreach ($File in Get-ChildItem -LiteralPath $InstallRoot -Recurse -File | Sort-Object FullName) {
    $Relative = Get-CompatibleRelativePath -BasePath $InstallRoot -FullPath $File.FullName
    $ManifestFiles += [ordered]@{
        path = $Relative
        sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$ManifestDlls = @(
    $ManifestFiles | Where-Object { $_.path -match '(?i)\.dll$' }
)
if ($ManifestDlls.Count -eq 0) {
    throw "No libsodium runtime DLL was staged"
}

$Manifest = [ordered]@{
    abi = 1
    component = "libsodium"
    version = $ExpectedVersion
    source_url = $OfficialSourceUrl
    archive_sha256 = $ExpectedArchiveSha256
    include_path = "include"
    library_path = "lib/libsodium.lib"
    dlls = $ManifestDlls
    files = $ManifestFiles
}
$ManifestPath = Join-Path $InstallRoot "runtime-manifest.json"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    $ManifestPath,
    ($Manifest | ConvertTo-Json -Depth 6),
    $Utf8NoBom
)

& (Join-Path $PSScriptRoot "verify_sodium_runtime.ps1") `
    -RuntimeRoot $InstallRoot `
    -ManifestPath $ManifestPath

Write-Host "CS_SODIUM_ROOT=$InstallRoot"
Write-Host "libsodium runtime manifest: $ManifestPath"
