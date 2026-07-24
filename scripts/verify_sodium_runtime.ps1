[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"
$ExpectedVersion = "1.0.22"
$ExpectedSourceUrl = "https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-msvc.zip"
$ExpectedArchiveSha256 = "3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279"
$ExpectedTreeSha256 = "9e6dc4f9e295621388418ca22ee1ee3bbfb0632af1287a18ce91ad1842af22be"
$ExpectedTreeFileCount = 79
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptRoot "canonical_tree_digest.ps1")
$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $RuntimeRoot "runtime-manifest.json"
}
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
$ExpectedManifestPath = [System.IO.Path]::GetFullPath(
    (Join-Path $RuntimeRoot "runtime-manifest.json")
)

function Resolve-ManifestPath {
    param([string]$RelativePath)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        $RelativePath.Contains('\') -or
        [System.IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|/)\.\.(/|$)') {
        throw "Invalid path in libsodium runtime manifest: $RelativePath"
    }
    $FullPath = [System.IO.Path]::GetFullPath(
        (Join-Path $RuntimeRoot $RelativePath.Replace(
            '/',
            [System.IO.Path]::DirectorySeparatorChar
        ))
    )
    $RootPrefix = $RuntimeRoot.TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith(
        $RootPrefix,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "Manifest path escapes libsodium runtime root: $RelativePath"
    }
    return $FullPath
}

if (-not (Test-Path -LiteralPath $RuntimeRoot -PathType Container)) {
    throw "libsodium runtime root is missing: $RuntimeRoot"
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "libsodium runtime manifest is missing: $ManifestPath"
}
if (-not $ManifestPath.Equals(
    $ExpectedManifestPath,
    [System.StringComparison]::OrdinalIgnoreCase
)) {
    throw "runtime-manifest.json is the only allowed untracked metadata"
}
$ManifestFile = Get-Item -LiteralPath $ManifestPath -Force
if (($ManifestFile.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "libsodium runtime manifest must not be a reparse point"
}

$CanonicalTree = Get-CanonicalTreeDigest `
    -Root $RuntimeRoot `
    -ExcludedRelativePaths @("runtime-manifest.json") `
    -AllowedExtensions @(".h", ".lib", ".dll")
if ($CanonicalTree.digest -ne $ExpectedTreeSha256 -or
    @($CanonicalTree.files).Count -ne $ExpectedTreeFileCount) {
    throw "libsodium runtime canonical tree does not match the verifier's pinned trust anchor"
}

$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($Manifest.abi -ne 1) {
    throw "Unsupported libsodium runtime manifest ABI"
}
if ($Manifest.component -ne "libsodium") {
    throw "libsodium runtime component mismatch"
}
if ($Manifest.version -ne $ExpectedVersion) {
    throw "libsodium runtime version mismatch"
}
if ($Manifest.source_url -ne $ExpectedSourceUrl) {
    throw "libsodium runtime source_url mismatch"
}
if ($Manifest.archive_sha256 -ne $ExpectedArchiveSha256) {
    throw "libsodium runtime archive_sha256 mismatch"
}
if ($Manifest.tree_sha256 -ne $ExpectedTreeSha256) {
    throw "libsodium runtime tree_sha256 mismatch"
}
if ($Manifest.include_path -ne "include") {
    throw "libsodium runtime include_path mismatch"
}
if ($Manifest.library_path -ne "lib/libsodium.lib") {
    throw "libsodium runtime library_path mismatch"
}

$IncludeRoot = Resolve-ManifestPath -RelativePath ([string]$Manifest.include_path)
$LibraryPath = Resolve-ManifestPath -RelativePath ([string]$Manifest.library_path)
if (-not (Test-Path -LiteralPath (Join-Path $IncludeRoot "sodium.h") -PathType Leaf)) {
    throw "libsodium public header is missing"
}
if (-not (Test-Path -LiteralPath $LibraryPath -PathType Leaf)) {
    throw "libsodium import library is missing"
}

$ExpectedFiles = @{}
foreach ($Entry in $Manifest.files) {
    $Relative = [string]$Entry.path
    $FullPath = Resolve-ManifestPath -RelativePath $Relative
    $Key = $Relative.ToLowerInvariant()
    if ($ExpectedFiles.ContainsKey($Key)) {
        throw "Duplicate path in libsodium runtime manifest: $Relative"
    }
    $Hash = [string]$Entry.sha256
    if ($Hash -notmatch '^[0-9a-f]{64}$') {
        throw "Invalid SHA256 in libsodium runtime manifest: $Relative"
    }
    $ExpectedFiles[$Key] = [ordered]@{
        path = $Relative
        full_path = $FullPath
        sha256 = $Hash
    }
}
if ($ExpectedFiles.Count -eq 0) {
    throw "libsodium runtime manifest contains no files"
}
if ($ExpectedFiles.Count -ne @($CanonicalTree.files).Count) {
    throw "libsodium runtime manifest does not enumerate the pinned canonical tree"
}
foreach ($Entry in @($CanonicalTree.files)) {
    $Key = ([string]$Entry.path).ToLowerInvariant()
    if (-not $ExpectedFiles.ContainsKey($Key) -or
        $ExpectedFiles[$Key].path -cne [string]$Entry.path -or
        $ExpectedFiles[$Key].sha256 -ne [string]$Entry.sha256) {
        throw "libsodium runtime manifest differs from the pinned canonical tree: $($Entry.path)"
    }
}

$ExpectedDlls = @{}
foreach ($Entry in $Manifest.dlls) {
    $Relative = [string]$Entry.path
    if ($Relative -notmatch '(?i)\.dll$') {
        throw "Non-DLL entry in libsodium runtime DLL list: $Relative"
    }
    Resolve-ManifestPath -RelativePath $Relative | Out-Null
    $Key = $Relative.ToLowerInvariant()
    if ($ExpectedDlls.ContainsKey($Key)) {
        throw "Duplicate DLL in libsodium runtime manifest: $Relative"
    }
    $Hash = [string]$Entry.sha256
    if ($Hash -notmatch '^[0-9a-f]{64}$') {
        throw "Invalid SHA256 for libsodium runtime DLL: $Relative"
    }
    if (-not $ExpectedFiles.ContainsKey($Key) -or
        $ExpectedFiles[$Key].sha256 -ne $Hash) {
        throw "libsodium DLL hash is not identical to the files manifest: $Relative"
    }
    $ExpectedDlls[$Key] = $Hash
}
if ($ExpectedDlls.Count -eq 0) {
    throw "libsodium runtime manifest contains no DLL hashes"
}

$ActualDlls = @(
    @($CanonicalTree.files) |
        Where-Object { $_.path -match '(?i)\.dll$' } |
        ForEach-Object { $_.path.ToLowerInvariant() }
)
if ($ActualDlls.Count -ne $ExpectedDlls.Count) {
    throw "missing or unexpected DLL in libsodium runtime"
}
foreach ($Key in $ActualDlls) {
    if (-not $ExpectedDlls.ContainsKey($Key)) {
        throw "unexpected DLL in libsodium runtime: $Key"
    }
}

Write-Host "Verified libsodium $ExpectedVersion runtime: $RuntimeRoot"
