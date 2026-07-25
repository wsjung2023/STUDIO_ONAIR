[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [Parameter(Mandatory = $true)]
    [ValidateSet("windows-x64")]
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

function Assert-ByteRange {
    param(
        [byte[]]$Bytes,
        [long]$Offset,
        [long]$Length,
        [string]$Description
    )
    if ($Offset -lt 0 -or $Length -lt 0 -or
        $Offset -gt $Bytes.LongLength -or
        $Length -gt ($Bytes.LongLength - $Offset)) {
        throw "Malformed Inochi2D PE: $Description"
    }
}

function Get-PeUInt16 {
    param([byte[]]$Bytes, [long]$Offset, [string]$Description)
    Assert-ByteRange $Bytes $Offset 2 $Description
    return [BitConverter]::ToUInt16($Bytes, [int]$Offset)
}

function Get-PeUInt32 {
    param([byte[]]$Bytes, [long]$Offset, [string]$Description)
    Assert-ByteRange $Bytes $Offset 4 $Description
    return [BitConverter]::ToUInt32($Bytes, [int]$Offset)
}

function Convert-PeRvaToFileOffset {
    param(
        [byte[]]$Bytes,
        [uint32]$Rva,
        [long]$Length,
        [uint32]$HeadersSize,
        [object[]]$Sections,
        [string]$Description
    )
    if ($Rva -lt $HeadersSize) {
        Assert-ByteRange $Bytes $Rva $Length $Description
        return [long]$Rva
    }
    foreach ($Section in $Sections) {
        [uint64]$MappedSize =
            [Math]::Max($Section.VirtualSize, $Section.RawSize)
        if ([uint64]$Rva -lt [uint64]$Section.VirtualAddress) {
            continue
        }
        [uint64]$Delta = [uint64]$Rva - [uint64]$Section.VirtualAddress
        if ($Delta -gt $MappedSize -or
            [uint64]$Length -gt ($MappedSize - $Delta) -or
            $Delta -gt [uint64]$Section.RawSize -or
            [uint64]$Length -gt ([uint64]$Section.RawSize - $Delta)) {
            continue
        }
        [uint64]$Offset = [uint64]$Section.RawOffset + $Delta
        Assert-ByteRange $Bytes ([long]$Offset) $Length $Description
        return [long]$Offset
    }
    throw "Malformed Inochi2D PE: $Description"
}

function Get-PeAsciiString {
    param([byte[]]$Bytes, [long]$Offset, [string]$Description)
    Assert-ByteRange $Bytes $Offset 1 $Description
    $Builder = [System.Text.StringBuilder]::new()
    for ([long]$Index = $Offset;
        $Index -lt $Bytes.LongLength -and $Builder.Length -le 1024;
        ++$Index) {
        $Value = $Bytes[$Index]
        if ($Value -eq 0) {
            if ($Builder.Length -eq 0) {
                throw "Malformed Inochi2D PE: $Description"
            }
            return $Builder.ToString()
        }
        if ($Value -lt 0x20 -or $Value -gt 0x7e) {
            throw "Malformed Inochi2D PE: $Description"
        }
        $Builder.Append([char]$Value) | Out-Null
    }
    throw "Malformed Inochi2D PE: $Description"
}

function Get-WindowsX64DllInfo {
    param([string]$Path, [switch]$SkipExports)
    $Item = Get-Item -LiteralPath $Path -Force
    if ($Item.Length -le 0 -or $Item.Length -gt 64MB) {
        throw "Inochi2D runtime binary has an invalid size"
    }
    [byte[]]$Bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($Bytes.Length -lt 64 -or
        (Get-PeUInt16 $Bytes 0 "DOS header") -ne 0x5a4d) {
        throw "Inochi2D runtime is not a PE image"
    }
    $PeOffset = Get-PeUInt32 $Bytes 0x3c "PE header offset"
    Assert-ByteRange $Bytes $PeOffset 24 "PE header"
    if ((Get-PeUInt32 $Bytes $PeOffset "PE signature") -ne 0x00004550) {
        throw "Inochi2D PE signature is invalid"
    }
    $Coff = [long]$PeOffset + 4
    $Machine = Get-PeUInt16 $Bytes $Coff "COFF machine"
    $SectionCount = Get-PeUInt16 $Bytes ($Coff + 2) "COFF section count"
    $OptionalSize = Get-PeUInt16 $Bytes ($Coff + 16) "optional header size"
    $Characteristics =
        Get-PeUInt16 $Bytes ($Coff + 18) "COFF characteristics"
    if ($Machine -ne 0x8664 -or $SectionCount -le 0 -or
        $SectionCount -gt 96 -or
        ($Characteristics -band 0x2002) -ne 0x2002) {
        throw "Inochi2D image is not an x64 DLL"
    }
    $Optional = $Coff + 20
    if ($OptionalSize -lt 128) {
        throw "Inochi2D image is not PE32+"
    }
    Assert-ByteRange $Bytes $Optional $OptionalSize "optional header"
    if ((Get-PeUInt16 $Bytes $Optional "optional header magic") -ne 0x020b) {
        throw "Inochi2D image is not PE32+"
    }
    $DirectoryCount =
        Get-PeUInt32 $Bytes ($Optional + 108) "data directory count"
    $HeadersSize = Get-PeUInt32 $Bytes ($Optional + 60) "header size"
    if ($DirectoryCount -lt 2 -or $HeadersSize -eq 0) {
        throw "Inochi2D PE data directories are missing"
    }
    $SectionTable = $Optional + $OptionalSize
    Assert-ByteRange $Bytes $SectionTable ($SectionCount * 40) "section table"
    $Sections = @()
    for ($Index = 0; $Index -lt $SectionCount; ++$Index) {
        $Offset = $SectionTable + $Index * 40
        $Section = [pscustomobject]@{
            VirtualSize =
                Get-PeUInt32 $Bytes ($Offset + 8) "section virtual size"
            VirtualAddress =
                Get-PeUInt32 $Bytes ($Offset + 12) "section virtual address"
            RawSize = Get-PeUInt32 $Bytes ($Offset + 16) "section raw size"
            RawOffset = Get-PeUInt32 $Bytes ($Offset + 20) "section raw offset"
        }
        Assert-ByteRange $Bytes $Section.RawOffset $Section.RawSize `
            "section raw data"
        $Sections += $Section
    }

    $ExportRva = Get-PeUInt32 $Bytes ($Optional + 112) "export RVA"
    $ExportSize = Get-PeUInt32 $Bytes ($Optional + 116) "export size"
    $ImportRva = Get-PeUInt32 $Bytes ($Optional + 120) "import RVA"
    $ImportSize = Get-PeUInt32 $Bytes ($Optional + 124) "import size"
    if (-not $SkipExports -and
        ($ExportRva -eq 0 -or $ExportSize -lt 40)) {
        throw "Inochi2D PE export directory is missing"
    }
    $Exports = [System.Collections.Generic.List[string]]::new()
    if (-not $SkipExports) {
        $ExportOffset = Convert-PeRvaToFileOffset $Bytes $ExportRva 40 `
            $HeadersSize $Sections "export directory"
        $NameCount =
            Get-PeUInt32 $Bytes ($ExportOffset + 24) "export name count"
        $NamesRva =
            Get-PeUInt32 $Bytes ($ExportOffset + 32) "export name table RVA"
        if ($NameCount -eq 0 -or $NameCount -gt 65536 -or $NamesRva -eq 0) {
            throw "Inochi2D PE export name table is invalid"
        }
        $NamesOffset = Convert-PeRvaToFileOffset $Bytes $NamesRva `
            ($NameCount * 4) $HeadersSize $Sections "export name table"
        for ($Index = 0; $Index -lt $NameCount; ++$Index) {
            $NameRva = Get-PeUInt32 $Bytes ($NamesOffset + $Index * 4) `
                "export name RVA"
            $NameOffset = Convert-PeRvaToFileOffset $Bytes $NameRva 1 `
                $HeadersSize $Sections "export name"
            $Exports.Add(
                (Get-PeAsciiString $Bytes $NameOffset "export name"))
        }
        if (@($Exports | Sort-Object -Unique).Count -ne $Exports.Count) {
            throw "Inochi2D PE contains duplicate exports"
        }
    }

    $Imports = [System.Collections.Generic.List[string]]::new()
    if ($ImportRva -ne 0 -or $ImportSize -ne 0) {
        if ($ImportRva -eq 0 -or $ImportSize -lt 20) {
            throw "Inochi2D PE import directory is invalid"
        }
        $MaximumDescriptors = [Math]::Min(
            [Math]::Floor($ImportSize / 20), 65536)
        $Terminated = $false
        for ($Index = 0; $Index -lt $MaximumDescriptors; ++$Index) {
            $DescriptorRva = [uint32]($ImportRva + $Index * 20)
            $Descriptor = Convert-PeRvaToFileOffset $Bytes $DescriptorRva 20 `
                $HeadersSize $Sections "import descriptor"
            $AllZero = $true
            for ($Field = 0; $Field -lt 5; ++$Field) {
                if ((Get-PeUInt32 $Bytes ($Descriptor + $Field * 4) `
                    "import descriptor") -ne 0) {
                    $AllZero = $false
                }
            }
            if ($AllZero) {
                $Terminated = $true
                break
            }
            $NameRva =
                Get-PeUInt32 $Bytes ($Descriptor + 12) "import name RVA"
            if ($NameRva -eq 0) {
                throw "Inochi2D PE import name is invalid"
            }
            $NameOffset = Convert-PeRvaToFileOffset $Bytes $NameRva 1 `
                $HeadersSize $Sections "import name"
            $Imports.Add(
                (Get-PeAsciiString $Bytes $NameOffset `
                    "import name").ToLowerInvariant())
        }
        if (-not $Terminated) {
            throw "Inochi2D PE import table is unterminated"
        }
    }
    return [pscustomobject]@{
        Exports = @($Exports | Sort-Object)
        Imports = @($Imports | Sort-Object -Unique)
    }
}

function Test-ApprovedWindowsImport {
    param([string]$Name)
    $Allowed = @(
        "advapi32.dll", "comctl32.dll", "kernel32.dll", "msvfw32.dll",
        "shell32.dll", "shlwapi.dll", "user32.dll", "vcruntime140.dll",
        "ws2_32.dll", "druntime-ldc-shared.dll",
        "phobos2-ldc-shared.dll"
    )
    return $Allowed -ccontains $Name -or
        ($Name.StartsWith("api-ms-win-") -and $Name.EndsWith(".dll"))
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
else {
    throw "Inochi2D binary inspection is not implemented for $ExpectedTarget"
}

$LibraryInfo = Get-WindowsX64DllInfo $LibraryPath
foreach ($Symbol in $RequiredSymbols) {
    if ($LibraryInfo.Exports -cnotcontains $Symbol) {
        throw "Inochi2D runtime DLL is missing required export: $Symbol"
    }
}
foreach ($Imported in $LibraryInfo.Imports) {
    if (-not (Test-ApprovedWindowsImport $Imported)) {
        throw "Inochi2D runtime DLL imports an unapproved library: $Imported"
    }
}
foreach ($Relative in $ExpectedWindowsLdcRuntimeHashes.Keys) {
    $DependencyPath = Resolve-ContainedPath $RuntimeRoot $Relative
    $DependencyInfo = Get-WindowsX64DllInfo $DependencyPath -SkipExports
    foreach ($Imported in $DependencyInfo.Imports) {
        if (-not (Test-ApprovedWindowsImport $Imported)) {
            throw (
                "Inochi2D dependency DLL imports an unapproved library: " +
                "$Imported")
        }
    }
}

Write-Host (
    "Verified audited Inochi2D $ExpectedVersion runtime: " +
    "$ExpectedTarget $LibrarySha256")
