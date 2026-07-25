[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [Parameter(Mandatory = $true)]
    [ValidateSet("windows-x64", "macos-arm64", "android-arm64")]
    [string]$ExpectedTarget
)

$ErrorActionPreference = "Stop"
$ExpectedVersion = "0.8.7-nightly+66fa768"
$ExpectedCommit = "66fa76834b28037db0c871c656563422f697879e"
$ExpectedArchiveSha256 =
    "79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb"
$ExpectedNoticeSha256 =
    "f79f6e26fa823e5c1881490bfee86627de43fc461ddeab4d80dc7af87cfc1743"
$ExpectedWindowsLdcRuntimeHashes = [ordered]@{
    "bin/druntime-ldc-shared.dll" =
        "f33033d32bb3f18c031fce39d02b9268389b121eb204f6237009e7cabbcf45ad"
    "bin/phobos2-ldc-shared.dll" =
        "25f313915a3b3b369eb65e529489459f7ebd24306edee3ef8d4bd4a9b7b3d4d4"
}
$RequiredSymbols = @(
    "in_puppet_load",
    "in_puppet_free",
    "in_puppet_get_parameters",
    "in_parameter_get_name",
    "in_parameter_get_dimensions",
    "in_parameter_set_value",
    "in_puppet_update",
    "in_puppet_draw",
    "in_puppet_get_drawlist",
    "in_drawlist_get_commands",
    "in_drawlist_get_vertex_data",
    "in_drawlist_get_index_data",
    "in_texture_get_width",
    "in_texture_get_height",
    "in_texture_get_channels",
    "in_texture_get_pixels"
)
$PinnedDependencies = [ordered]@{
    "imagefmt" = @("2.1.2", "10f4182efc4fc3846561ca702b3207493f736639498b2dc61a3adcee2bb18736")
    "inmath" = @("1.3.0", "865fa85d6c07c5f23207cdf9987207d95547e8303009cd0a028b8e7aa9d5aeae")
    "intel-intrinsics" = @("1.12.1", "4e056612b6ebe819fef2e45c19d78427b7e67b3c8650445e7379ed2b30f61519")
    "nulib" = @("0.3.5", "e4b56c28cd3264c72ba18e21889b9dddd1927b83828d92ebe6b49d559b22e597")
    "numem" = @("1.3.2", "771688ea0ac4990e8576de4cdcdb381449d78d9edf7a6a7d55adeccfe46d94cc")
    "silly" = @("1.1.1", "ffb78e740db5ab36c216c349ec36548a91c66fd1b69b980c1fd3e912ce8ae73b")
}
$TargetPolicy = @{
    "windows-x64" = @{
        triple = "x86_64-pc-windows-msvc"
        minimum = "windows-10-1809"
        library = "bin/inochi2d.dll"
    }
    "macos-arm64" = @{
        triple = "arm64-apple-darwin"
        minimum = "macos-13.0"
        library = "lib/libinochi2d.dylib"
    }
    "android-arm64" = @{
        triple = "aarch64-linux-android26"
        minimum = "android-api-26"
        library = "lib/libinochi2d.so"
    }
}

function Resolve-ContainedPath {
    param([string]$Root, [string]$Relative)
    if ([string]::IsNullOrWhiteSpace($Relative) -or
        $Relative.Contains('\') -or
        [System.IO.Path]::IsPathRooted($Relative) -or
        $Relative -match '(^|/)\.\.(/|$)' -or
        $Relative -match '(^|/)\.(/|$)') {
        throw "Invalid path in Inochi2D runtime manifest: $Relative"
    }
    $FullPath = [System.IO.Path]::GetFullPath(
        (Join-Path $Root $Relative.Replace(
            '/', [System.IO.Path]::DirectorySeparatorChar)))
    $Prefix = $Root.TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith(
        $Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Inochi2D manifest path escapes its runtime root: $Relative"
    }
    return $FullPath
}

function Get-CompatibleRelativePath {
    param([string]$Root, [string]$FullPath)
    $Base = $Root.TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    $BaseUri = [System.Uri]::new($Base)
    $FileUri = [System.Uri]::new([System.IO.Path]::GetFullPath($FullPath))
    return [System.Uri]::UnescapeDataString(
        $BaseUri.MakeRelativeUri($FileUri).ToString())
}

function Assert-RegularUnredirectedFile {
    param([string]$Path, [string]$Description)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $Item = Get-Item -LiteralPath $Path -Force
    if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a reparse point: $Path"
    }
}

function Read-FilePrefix {
    param([string]$Path, [int]$Count = 512)
    $Stream = [System.IO.File]::Open(
        $Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    try {
        $Buffer = New-Object byte[] $Count
        $Read = $Stream.Read($Buffer, 0, $Buffer.Length)
        if ($Read -eq $Buffer.Length) { return $Buffer }
        $Result = New-Object byte[] $Read
        [Array]::Copy($Buffer, $Result, $Read)
        return $Result
    }
    finally {
        $Stream.Dispose()
    }
}

function Assert-TargetArchitecture {
    param([string]$Path, [string]$Target)
    [byte[]]$Bytes = Read-FilePrefix -Path $Path
    if ($Target -eq "windows-x64") {
        if ($Bytes.Length -lt 70 -or $Bytes[0] -ne 0x4d -or
            $Bytes[1] -ne 0x5a) {
            throw "Inochi2D runtime is not a PE library"
        }
        $PeOffset = [BitConverter]::ToUInt32($Bytes, 0x3c)
        if ($PeOffset -gt ($Bytes.Length - 6) -or
            $Bytes[$PeOffset] -ne 0x50 -or
            $Bytes[$PeOffset + 1] -ne 0x45 -or
            $Bytes[$PeOffset + 2] -ne 0 -or
            $Bytes[$PeOffset + 3] -ne 0 -or
            [BitConverter]::ToUInt16($Bytes, $PeOffset + 4) -ne 0x8664) {
            throw "Inochi2D runtime is not an x64 MSVC-compatible PE library"
        }
        return
    }
    if ($Target -eq "macos-arm64") {
        $Expected = @(0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0x00, 0x00, 0x01)
        if ($Bytes.Length -lt 8) {
            throw "Inochi2D runtime is not an arm64 Mach-O library"
        }
        for ($Index = 0; $Index -lt $Expected.Count; ++$Index) {
            if ($Bytes[$Index] -ne $Expected[$Index]) {
                throw "Inochi2D runtime is not an arm64 Mach-O library"
            }
        }
        return
    }
    if ($Bytes.Length -lt 20 -or $Bytes[0] -ne 0x7f -or
        $Bytes[1] -ne 0x45 -or $Bytes[2] -ne 0x4c -or
        $Bytes[3] -ne 0x46 -or $Bytes[4] -ne 2 -or $Bytes[5] -ne 1 -or
        [BitConverter]::ToUInt16($Bytes, 18) -ne 183) {
        throw "Inochi2D runtime is not an arm64 ELF library"
    }
}

$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
if (-not (Test-Path -LiteralPath $RuntimeRoot -PathType Container)) {
    throw "Inochi2D runtime root is missing: $RuntimeRoot"
}
$RootItem = Get-Item -LiteralPath $RuntimeRoot -Force
if (($RootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Inochi2D runtime root must not be a reparse point"
}

$ManifestPath = Join-Path $RuntimeRoot "runtime-manifest.json"
Assert-RegularUnredirectedFile $ManifestPath "Inochi2D runtime manifest"
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8 |
    ConvertFrom-Json
$Policy = $TargetPolicy[$ExpectedTarget]
if ($Manifest.schema_version -ne 1 -or
    $Manifest.component -ne "Inochi2D C-FFI" -or
    $Manifest.version -ne $ExpectedVersion -or
    $Manifest.source_commit -ne $ExpectedCommit -or
    $Manifest.source_archive_sha256 -ne $ExpectedArchiveSha256 -or
    $Manifest.license -ne "BSD-2-Clause" -or
    $Manifest.linking -ne "dynamic" -or
    $Manifest.target -ne $ExpectedTarget -or
    $Manifest.target_triple -ne $Policy.triple -or
    $Manifest.minimum_platform -ne $Policy.minimum -or
    $Manifest.abi_mode -ne "IN_VEC2_POSITION" -or
    [string]::IsNullOrWhiteSpace([string]$Manifest.compiler) -or
    [string]::IsNullOrWhiteSpace([string]$Manifest.sdk)) {
    throw "Inochi2D runtime identity is not approved"
}
if (@($Manifest.dependencies.PSObject.Properties).Count -ne
    $PinnedDependencies.Count) {
    throw "Inochi2D runtime dependency set is not approved"
}
if ($ExpectedTarget -eq "windows-x64") {
    $RuntimeDependencies = @($Manifest.runtime_dependencies)
    if ($RuntimeDependencies.Count -ne
        $ExpectedWindowsLdcRuntimeHashes.Count) {
        throw "Inochi2D LDC runtime dependency set is not approved"
    }
    foreach ($ExpectedPath in $ExpectedWindowsLdcRuntimeHashes.Keys) {
        $Matches = @(
            $RuntimeDependencies |
            Where-Object { $_.path -ceq $ExpectedPath }
        )
        if ($Matches.Count -ne 1 -or
            $Matches[0].sha256 -cne
                $ExpectedWindowsLdcRuntimeHashes[$ExpectedPath] -or
            $Matches[0].component -cne "LDC 1.40.0 BSL-1.0 runtime") {
            throw "Inochi2D LDC runtime dependency is not approved: $ExpectedPath"
        }
    }
}
elseif (@($Manifest.runtime_dependencies).Count -ne 0) {
    throw "Inochi2D runtime declares an unapproved target dependency"
}
foreach ($Name in $PinnedDependencies.Keys) {
    $Expected = $PinnedDependencies[$Name]
    $Actual = $Manifest.dependencies.$Name
    if (@($Actual.PSObject.Properties).Count -ne 2 -or
        $Actual.version -ne $Expected[0] -or
        $Actual.archive_sha256 -ne $Expected[1]) {
        throw "Inochi2D runtime dependency set is not approved: $Name"
    }
}
if ($Manifest.library.path -ne $Policy.library -or
    [string]$Manifest.library.sha256 -notmatch '^[0-9a-f]{64}$' -or
    $Manifest.notice.path -ne "LICENSE" -or
    $Manifest.notice.sha256 -ne $ExpectedNoticeSha256 -or
    $Manifest.third_party_notices.path -ne "THIRD_PARTY_NOTICES.txt" -or
    [string]$Manifest.third_party_notices.sha256 -notmatch '^[0-9a-f]{64}$') {
    throw "Inochi2D runtime artifact identity is not approved"
}

$Symbols = @($Manifest.symbols)
if ($Symbols.Count -ne $RequiredSymbols.Count -or
    @($Symbols | Select-Object -Unique).Count -ne $RequiredSymbols.Count) {
    throw "Inochi2D runtime symbol list is not approved"
}
foreach ($Symbol in $RequiredSymbols) {
    if ($Symbols -cnotcontains $Symbol) {
        throw "Inochi2D runtime is missing required symbol: $Symbol"
    }
}

$LibraryPath = Resolve-ContainedPath $RuntimeRoot $Policy.library
$NoticePath = Resolve-ContainedPath $RuntimeRoot "LICENSE"
$ThirdPartyNoticesPath =
    Resolve-ContainedPath $RuntimeRoot "THIRD_PARTY_NOTICES.txt"
Assert-RegularUnredirectedFile $LibraryPath "Inochi2D runtime library"
Assert-RegularUnredirectedFile $NoticePath "Inochi2D BSD notice"
Assert-RegularUnredirectedFile $ThirdPartyNoticesPath `
    "Inochi2D third-party notices"

$ExpectedFiles = @(
    "runtime-manifest.json",
    $Policy.library,
    "LICENSE",
    "THIRD_PARTY_NOTICES.txt"
)
if ($ExpectedTarget -eq "windows-x64") {
    $ExpectedFiles += @($ExpectedWindowsLdcRuntimeHashes.Keys)
}
$ActualFiles = @()
foreach ($Item in Get-ChildItem -LiteralPath $RuntimeRoot -Recurse -Force) {
    if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Inochi2D runtime contains a redirected artifact: $($Item.FullName)"
    }
    if (-not $Item.PSIsContainer) {
        $ActualFiles += (Get-CompatibleRelativePath $RuntimeRoot $Item.FullName)
    }
}
if ($ActualFiles.Count -ne $ExpectedFiles.Count) {
    throw "Inochi2D runtime contains an unexpected staged artifact"
}
foreach ($Relative in $ActualFiles) {
    if ($ExpectedFiles -cnotcontains $Relative) {
        throw "Inochi2D runtime contains an unexpected staged artifact: $Relative"
    }
}

$LibrarySha256 =
    (Get-FileHash -LiteralPath $LibraryPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($LibrarySha256 -ne $Manifest.library.sha256) {
    throw "Inochi2D runtime library SHA256 mismatch"
}
$NoticeSha256 =
    (Get-FileHash -LiteralPath $NoticePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($NoticeSha256 -ne $ExpectedNoticeSha256) {
    throw "Inochi2D BSD notice SHA256 mismatch"
}
$ThirdPartyNoticesSha256 = (Get-FileHash -LiteralPath $ThirdPartyNoticesPath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ThirdPartyNoticesSha256 -ne
    $Manifest.third_party_notices.sha256) {
    throw "Inochi2D third-party notices SHA256 mismatch"
}
if ($ExpectedTarget -eq "windows-x64") {
    foreach ($Relative in $ExpectedWindowsLdcRuntimeHashes.Keys) {
        $DependencyPath = Resolve-ContainedPath $RuntimeRoot $Relative
        Assert-RegularUnredirectedFile $DependencyPath `
            "Inochi2D LDC runtime dependency"
        $DependencyHash = (Get-FileHash -LiteralPath $DependencyPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($DependencyHash -ne $ExpectedWindowsLdcRuntimeHashes[$Relative]) {
            throw "Inochi2D LDC runtime dependency SHA256 mismatch: $Relative"
        }
    }
}
Assert-TargetArchitecture $LibraryPath $ExpectedTarget

Write-Host (
    "Verified audited Inochi2D $ExpectedVersion runtime: " +
    "$ExpectedTarget $LibrarySha256")
