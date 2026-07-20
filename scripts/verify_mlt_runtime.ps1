[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"
$ExpectedVersion = "7.40.0"
$ExpectedSourceCommit = "bef9d89c0c279e558d9625dac3399c2aa3d961bc"
$ExpectedWindowsMutexPatch = "creator-studio-pthreads4w-v3.0.0-lazy-mutex-events-v1"
$ExpectedFfmpegVersion = "8.1.2"
$ExpectedFfmpegArchiveSha256 = "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
$ExpectedVcpkgCommit = "43643e1f5cf73db40d0d4bd610183348eb09b24e"
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
if ($Manifest.windows_mutex_patch -ne $ExpectedWindowsMutexPatch) {
    throw "MLT runtime windows_mutex_patch mismatch"
}
if ($Manifest.abi -ne 1) {
    throw "Unsupported MLT runtime manifest ABI"
}

$ApprovedDependencies = @(
    [pscustomobject]@{ component = "FFmpeg"; version = $ExpectedFfmpegVersion; source_identity = "sha256:$ExpectedFfmpegArchiveSha256"; license = "LGPL-2.1-or-later" }
    [pscustomobject]@{ component = "zlib"; version = "1.3.2"; source_identity = "vcpkg:$ExpectedVcpkgCommit"; license = "Zlib" }
    [pscustomobject]@{ component = "PThreads4W"; version = "3.0.0"; source_identity = "vcpkg:$ExpectedVcpkgCommit;patch:$ExpectedWindowsMutexPatch"; license = "Apache-2.0" }
    [pscustomobject]@{ component = "GNU libiconv"; version = "1.19"; source_identity = "vcpkg:$ExpectedVcpkgCommit"; license = "LGPL-2.1-or-later" }
    [pscustomobject]@{ component = "dlfcn-win32"; version = "1.4.2"; source_identity = "vcpkg:$ExpectedVcpkgCommit"; license = "MIT" }
)
if ($Manifest.dependencies.Count -ne $ApprovedDependencies.Count) {
    throw "MLT runtime dependency identity set is incomplete"
}
for ($Index = 0; $Index -lt $ApprovedDependencies.Count; ++$Index) {
    $ActualDependency = $Manifest.dependencies[$Index]
    $ExpectedDependency = $ApprovedDependencies[$Index]
    if ($ActualDependency.component -ne $ExpectedDependency.component -or
        $ActualDependency.version -ne $ExpectedDependency.version -or
        $ActualDependency.source_identity -ne $ExpectedDependency.source_identity -or
        $ActualDependency.license -ne $ExpectedDependency.license) {
        throw "MLT runtime dependency identity is not approved"
    }
}

function Get-ApprovedProvenance {
    param([string]$Relative)
    $Lower = $Relative.ToLowerInvariant()
    $Name = [System.IO.Path]::GetFileName($Lower)
    $VcpkgIdentity = "vcpkg:$ExpectedVcpkgCommit"
    if ($Name -match '^(avcodec-|avfilter-|avformat-|avutil-|swresample-|swscale-).*\.dll$') {
        return @("FFmpeg", $ExpectedFfmpegVersion, "sha256:$ExpectedFfmpegArchiveSha256", "LGPL-2.1-or-later")
    }
    if ($Name -match '^z\.dll$') {
        return @("zlib", "1.3.2", $VcpkgIdentity, "Zlib")
    }
    if ($Name -match '^pthread.*\.dll$' -or $Lower.StartsWith("include/mlt-deps/")) {
        return @("PThreads4W", "3.0.0", "$VcpkgIdentity;patch:$ExpectedWindowsMutexPatch", "Apache-2.0")
    }
    if ($Name -in @("iconv-2.dll", "libiconv-2.dll")) {
        return @("GNU libiconv", "1.19", $VcpkgIdentity, "LGPL-2.1-or-later")
    }
    if ($Name -eq "dl.dll") {
        return @("dlfcn-win32", "1.4.2", $VcpkgIdentity, "MIT")
    }
    if ($Lower -eq "creator-studio-mlt-build.txt") {
        return @("Creator Studio", "1", "repository:R1-03", "LicenseRef-Creator-Studio-Proprietary")
    }
    if ($Lower.StartsWith("bin/mlt") -or $Lower.StartsWith("lib/") -or
        $Lower.StartsWith("share/") -or $Lower.StartsWith("include/mlt-7/")) {
        return @("MLT Framework", $ExpectedVersion, $ExpectedSourceCommit, "LGPL-2.1-or-later")
    }
    throw "No approved provenance classification for MLT artifact: $Relative"
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
    $Approved = Get-ApprovedProvenance $Relative
    if ($Entry.component -ne $Approved[0] -or $Entry.version -ne $Approved[1] -or
        $Entry.source_identity -ne $Approved[2] -or $Entry.license -ne $Approved[3]) {
        throw "Unapproved file provenance in MLT runtime manifest: $Relative"
    }
    $Expected[$Relative] = [string]$Entry.sha256
}

foreach ($Required in @(
    "bin/mlt-7.dll",
    "bin/mlt++-7.dll",
    "bin/z.dll",
    "lib/mlt-7/mltcore.dll",
    "lib/mlt-7/mltavformat.dll"
)) {
    if (-not $Expected.ContainsKey($Required)) {
        throw "MLT runtime is missing a required approved component: $Required"
    }
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
