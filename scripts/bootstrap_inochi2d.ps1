[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("windows-x64", "macos-arm64", "android-arm64")]
    [string]$Target,
    [string]$BuildRoot = "",
    [string]$InstallRoot = "",
    [string]$OfficialArchivePath = "",
    [string]$LdcRoot = "",
    [string]$AndroidNdkRoot = ""
)

# Source-build the pre-0.9 C FFI from one immutable, signed upstream Nightly
# commit. The v0.8.7 tag was audited and rejected because it has no dynamic
# configuration or C FFI. Do not replace this with a prebuilt Nightly binary.
$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$ExpectedVersion = "0.8.7-nightly+66fa768"
$ExpectedSourceCommit = "66fa76834b28037db0c871c656563422f697879e"
$ExpectedSourceArchiveSha256 =
    "79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb"
$ExpectedLicenseSha256 =
    "f79f6e26fa823e5c1881490bfee86627de43fc461ddeab4d80dc7af87cfc1743"
$OfficialSourceArchiveUrl =
    "https://github.com/Inochi2D/inochi2d/archive/66fa76834b28037db0c871c656563422f697879e.tar.gz"
$RequiredLdcVersion = "1.40.0"
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
    "imagefmt" = "2.1.2"
    "inmath" = "1.3.0"
    "intel-intrinsics" = "1.12.1"
    "nulib" = "0.3.5"
    "numem" = "1.3.2"
    "silly" = "1.1.1"
}
$PinnedDependencyHashes = [ordered]@{
    "imagefmt" = "10f4182efc4fc3846561ca702b3207493f736639498b2dc61a3adcee2bb18736"
    "inmath" = "865fa85d6c07c5f23207cdf9987207d95547e8303009cd0a028b8e7aa9d5aeae"
    "intel-intrinsics" = "4e056612b6ebe819fef2e45c19d78427b7e67b3c8650445e7379ed2b30f61519"
    "nulib" = "e4b56c28cd3264c72ba18e21889b9dddd1927b83828d92ebe6b49d559b22e597"
    "numem" = "771688ea0ac4990e8576de4cdcdb381449d78d9edf7a6a7d55adeccfe46d94cc"
    "silly" = "ffb78e740db5ab36c216c349ec36548a91c66fd1b69b980c1fd3e912ce8ae73b"
}
$DependencyLicenses = [ordered]@{
    "imagefmt" = "BSD-2-Clause"
    "inmath" = "MIT"
    "intel-intrinsics" = "BSL-1.0"
    "nulib" = "BSL-1.0"
    "numem" = "BSL-1.0"
    "silly" = "ISC"
}
$DependencyLicenseFiles = [ordered]@{
    "imagefmt" = "LICENSE"
    "inmath" = "LICENSE"
    "intel-intrinsics" = "COPYING"
    "nulib" = "LICENSE"
    "numem" = "LICENSE"
    "silly" = "LICENSE"
}

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepositoryRoot "build/inochi2d"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $BuildRoot "prefix/$Target"
}
if ([string]::IsNullOrWhiteSpace($OfficialArchivePath)) {
    $OfficialArchivePath = Join-Path $RepositoryRoot `
        "build/downloads/inochi2d-$ExpectedSourceCommit.tar.gz"
}
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$OfficialArchivePath = [System.IO.Path]::GetFullPath($OfficialArchivePath)
$SourceRoot = Join-Path $BuildRoot "source-$Target"
$DubHome = Join-Path $BuildRoot "dub-home"
$DependencyArchiveRoot = Join-Path $BuildRoot "dependency-archives"

function Assert-StrictChildPath {
    param([string]$Parent, [string]$Child, [string]$Description)
    $ParentPrefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    $ResolvedChild = [System.IO.Path]::GetFullPath($Child)
    if (-not $ResolvedChild.StartsWith(
        $ParentPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description must remain inside $Parent"
    }
}

Assert-StrictChildPath $BuildRoot $SourceRoot "Inochi2D source root"
Assert-StrictChildPath $BuildRoot $InstallRoot "Inochi2D install root"
Assert-StrictChildPath $BuildRoot $DubHome "Inochi2D DUB cache"
Assert-StrictChildPath $BuildRoot $DependencyArchiveRoot `
    "Inochi2D dependency archive root"

if (-not [string]::IsNullOrWhiteSpace($LdcRoot)) {
    $LdcRoot = [System.IO.Path]::GetFullPath($LdcRoot)
    $LdcBin = Join-Path $LdcRoot "bin"
    $env:Path = "$LdcBin$([System.IO.Path]::PathSeparator)$env:Path"
}
$Ldc = Get-Command ldc2 -ErrorAction SilentlyContinue
$Dub = Get-Command dub -ErrorAction SilentlyContinue
if (-not $Ldc -or -not $Dub) {
    throw "LDC $RequiredLdcVersion and DUB are required; pass -LdcRoot"
}
$CompilerLines = @(& $Ldc.Source --version)
if ($LASTEXITCODE -ne 0 -or
    ($CompilerLines -join "`n") -notmatch `
        "LDC - the LLVM D compiler \($([regex]::Escape($RequiredLdcVersion))\)") {
    throw "The audited build requires exact LDC $RequiredLdcVersion"
}
$CompilerIdentity = ($CompilerLines | Select-Object -First 4) -join "; "
$DubVersion = (& $Dub.Source --version) -join " "
if ($LASTEXITCODE -ne 0) { throw "Could not identify DUB" }

if (-not (Test-Path -LiteralPath $OfficialArchivePath -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path `
        (Split-Path -Parent $OfficialArchivePath) | Out-Null
    $DownloadPath = "$OfficialArchivePath.download"
    Invoke-WebRequest -Uri $OfficialSourceArchiveUrl -OutFile $DownloadPath
    Move-Item -LiteralPath $DownloadPath -Destination $OfficialArchivePath
}
$ArchiveHash = (Get-FileHash -LiteralPath $OfficialArchivePath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $ExpectedSourceArchiveSha256) {
    throw "Official Inochi2D source archive hash mismatch: $ArchiveHash"
}

if (Test-Path -LiteralPath $SourceRoot) {
    Assert-StrictChildPath $BuildRoot $SourceRoot "Inochi2D source root"
    Remove-Item -LiteralPath $SourceRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $SourceRoot | Out-Null
tar -xf $OfficialArchivePath -C $SourceRoot --strip-components=1
if ($LASTEXITCODE -ne 0) {
    throw "Could not extract the pinned Inochi2D source"
}
$DubRecipePath = Join-Path $SourceRoot "dub.sdl"
$LicensePath = Join-Path $SourceRoot "LICENSE"
$VersionPath = Join-Path $SourceRoot "source/inochi2d/ver.d"
foreach ($RequiredPath in @($DubRecipePath, $LicensePath, $VersionPath)) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "Pinned Inochi2D source is incomplete: $RequiredPath"
    }
}
$Recipe = Get-Content -LiteralPath $DubRecipePath -Raw -Encoding utf8
$VersionSource = Get-Content -LiteralPath $VersionPath -Raw -Encoding utf8
if ($Recipe -notmatch 'configuration\s+"dynamic"' -or
    $Recipe -notmatch 'targetType\s+"dynamicLibrary"' -or
    $Recipe -notmatch 'versions\s+"IN_DYNLIB"' -or
    $VersionSource -notmatch 'IN_VERSION\s*=\s*"v0\.8\.7"') {
    throw "Pinned Inochi2D source does not contain the audited pre-0.9 C FFI"
}
$LicenseHash = (Get-FileHash -LiteralPath $LicensePath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($LicenseHash -ne $ExpectedLicenseSha256) {
    throw "Pinned Inochi2D BSD-2-Clause notice changed"
}
foreach ($Symbol in $RequiredSymbols) {
    if (-not (Select-String -Path (Join-Path $SourceRoot "source/inochi2d/cffi/*.d") `
        -Pattern "\b$([regex]::Escape($Symbol))\s*\(" -Quiet)) {
        throw "Pinned Inochi2D source omits required C FFI symbol: $Symbol"
    }
}

$Selections = [ordered]@{
    fileVersion = 1
    versions = $PinnedDependencies
}
$SelectionsPath = Join-Path $SourceRoot "dub.selections.json"
[System.IO.File]::WriteAllText(
    $SelectionsPath, ($Selections | ConvertTo-Json -Depth 4),
    [System.Text.UTF8Encoding]::new($false))

# Prime an empty, build-local DUB cache exclusively from package archives whose
# bytes are pinned here. --skip-registry=all below prevents any fallback to an
# unconstrained registry or user cache.
if (Test-Path -LiteralPath $DubHome) {
    Assert-StrictChildPath $BuildRoot $DubHome "Inochi2D DUB cache"
    Remove-Item -LiteralPath $DubHome -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $DependencyArchiveRoot | Out-Null
foreach ($Name in $PinnedDependencies.Keys) {
    $Version = $PinnedDependencies[$Name]
    $ArchivePath = Join-Path $DependencyArchiveRoot "$Name-$Version.zip"
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
        $DownloadPath = "$ArchivePath.download"
        Invoke-WebRequest `
            -Uri "https://code.dlang.org/packages/$Name/$Version.zip" `
            -OutFile $DownloadPath
        Move-Item -LiteralPath $DownloadPath -Destination $ArchivePath
    }
    $PackageHash = (Get-FileHash -LiteralPath $ArchivePath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($PackageHash -ne $PinnedDependencyHashes[$Name]) {
        throw "DUB package archive hash mismatch: $Name $Version"
    }
    $PackageParent = Join-Path $DubHome "packages/$Name/$Version"
    $PackageRoot = Join-Path $PackageParent $Name
    New-Item -ItemType Directory -Force -Path $PackageRoot | Out-Null
    tar -xf $ArchivePath -C $PackageRoot --strip-components=1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not extract verified DUB package: $Name $Version"
    }
    # The DUB registry normally injects its immutable selected version into the
    # fetched recipe. Because this bootstrap bypasses the registry after
    # verifying the official archive, reproduce only that metadata injection.
    $JsonRecipe = Join-Path $PackageRoot "dub.json"
    $SdlRecipe = Join-Path $PackageRoot "dub.sdl"
    if (Test-Path -LiteralPath $JsonRecipe -PathType Leaf) {
        $RecipeObject = Get-Content -LiteralPath $JsonRecipe -Raw `
            -Encoding utf8 | ConvertFrom-Json
        $RecipeObject | Add-Member -NotePropertyName version `
            -NotePropertyValue $Version -Force
        [System.IO.File]::WriteAllText(
            $JsonRecipe, ($RecipeObject | ConvertTo-Json -Depth 20),
            [System.Text.UTF8Encoding]::new($false))
    }
    elseif (Test-Path -LiteralPath $SdlRecipe -PathType Leaf) {
        $RecipeText = Get-Content -LiteralPath $SdlRecipe -Raw -Encoding utf8
        [System.IO.File]::WriteAllText(
            $SdlRecipe, "version `"$Version`"`r`n$RecipeText",
            [System.Text.UTF8Encoding]::new($false))
    }
    else {
        throw "Verified DUB package has no recipe: $Name $Version"
    }
    New-Item -ItemType File -Force -Path (Join-Path $PackageParent ".lock") |
        Out-Null
}
$env:DUB_HOME = $DubHome

$TargetTriple = ""
$MinimumPlatform = ""
$BuiltLibrary = ""
$StagedRelative = ""
$SdkIdentity = ""
$SymbolTool = ""
$SymbolArguments = @()
$BuildArguments = @(
    "build",
    "--config=dynamic",
    "--compiler=ldc2",
    "--build=release",
    "--skip-registry=all"
)

if ($Target -eq "windows-x64") {
    if ($env:OS -ne "Windows_NT" -or
        $env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
        throw "windows-x64 must be source-built on an x64 Windows host"
    }
    $BuildArguments += "--arch=x86_64"
    $TargetTriple = "x86_64-pc-windows-msvc"
    $MinimumPlatform = "windows-10-1809"
    $BuiltLibrary = Join-Path $SourceRoot "out/inochi2d.dll"
    $StagedRelative = "bin/inochi2d.dll"

    if (-not (Get-Command dumpbin -ErrorAction SilentlyContinue)) {
        $VcVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\" +
            "BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        if (-not (Test-Path -LiteralPath $VcVars)) {
            throw "Visual Studio 2022 x64 SDK tools were not found"
        }
        $EnvironmentLines = & cmd.exe /d /s /c `
            "call `"$VcVars`" >nul 2>&1 && set"
        if ($LASTEXITCODE -ne 0) {
            throw "Could not initialize the MSVC x64 SDK tools"
        }
        foreach ($Line in $EnvironmentLines) {
            $Separator = $Line.IndexOf('=')
            if ($Separator -gt 0) {
                [Environment]::SetEnvironmentVariable(
                    $Line.Substring(0, $Separator),
                    $Line.Substring($Separator + 1),
                    [EnvironmentVariableTarget]::Process)
            }
        }
    }
    $SymbolTool = (Get-Command dumpbin -ErrorAction Stop).Source
    $SymbolArguments = @("/nologo", "/exports")
    if ([string]::IsNullOrWhiteSpace($env:WindowsSDKVersion) -or
        [string]::IsNullOrWhiteSpace($env:VCToolsVersion)) {
        throw "Windows SDK and MSVC tool identities are required"
    }
    $SdkIdentity =
        "Windows SDK $($env:WindowsSDKVersion.TrimEnd('\')); " +
        "MSVC $($env:VCToolsVersion.TrimEnd('\'))"
}
elseif ($Target -eq "macos-arm64") {
    if (-not (Get-Command sw_vers -ErrorAction SilentlyContinue) -or
        (& uname -m) -ne "arm64") {
        throw "macos-arm64 must be source-built on an arm64 macOS host"
    }
    $BuildArguments += "--arch=arm64-apple-macos"
    $env:MACOSX_DEPLOYMENT_TARGET = "13.0"
    $TargetTriple = "arm64-apple-darwin"
    $MinimumPlatform = "macos-13.0"
    $BuiltLibrary = Join-Path $SourceRoot "out/libinochi2d.dylib"
    $StagedRelative = "lib/libinochi2d.dylib"
    $SymbolTool = (Get-Command nm -ErrorAction Stop).Source
    $SymbolArguments = @("-gU")
    $SdkVersion = (& xcrun --sdk macosx --show-sdk-version) -join " "
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($SdkVersion)) {
        throw "Could not identify the checked macOS SDK"
    }
    $SdkIdentity = "macOS SDK $SdkVersion; deployment target 13.0"
}
else {
    if ([string]::IsNullOrWhiteSpace($AndroidNdkRoot)) {
        $AndroidNdkRoot = $env:ANDROID_NDK_HOME
    }
    if ([string]::IsNullOrWhiteSpace($AndroidNdkRoot)) {
        throw "android-arm64 requires -AndroidNdkRoot or ANDROID_NDK_HOME"
    }
    $AndroidNdkRoot = [System.IO.Path]::GetFullPath($AndroidNdkRoot)
    $PropertiesPath = Join-Path $AndroidNdkRoot "source.properties"
    if (-not (Test-Path -LiteralPath $PropertiesPath -PathType Leaf)) {
        throw "Android NDK source.properties is missing"
    }
    $HostTag = if ($env:OS -eq "Windows_NT") { "windows-x86_64" }
        elseif ((& uname -s) -eq "Darwin") { "darwin-x86_64" }
        else { "linux-x86_64" }
    $ToolchainBin = Join-Path $AndroidNdkRoot `
        "toolchains/llvm/prebuilt/$HostTag/bin"
    $AndroidClang = Join-Path $ToolchainBin `
        $(if ($env:OS -eq "Windows_NT") {
            "aarch64-linux-android26-clang.cmd"
        } else {
            "aarch64-linux-android26-clang"
        })
    $AndroidNm = Join-Path $ToolchainBin `
        $(if ($env:OS -eq "Windows_NT") { "llvm-nm.exe" } else { "llvm-nm" })
    foreach ($Tool in @($AndroidClang, $AndroidNm)) {
        if (-not (Test-Path -LiteralPath $Tool -PathType Leaf)) {
            throw "Checked Android NDK API 26 tool is missing: $Tool"
        }
    }
    $TargetProbe = (& $Ldc.Source -mtriple=aarch64-linux-android26 --version) `
        -join "`n"
    if ($LASTEXITCODE -ne 0 -or
        $TargetProbe -notmatch 'aarch64.*android26') {
        throw "LDC lacks the checked Android arm64 API 26 cross target"
    }
    $BuildArguments += "--arch=aarch64-linux-android26"
    $env:CC = $AndroidClang
    $env:CXX = $AndroidClang.Replace("clang", "clang++")
    $env:DFLAGS = "-mtriple=aarch64-linux-android26 -gcc=$AndroidClang"
    $TargetTriple = "aarch64-linux-android26"
    $MinimumPlatform = "android-api-26"
    $BuiltLibrary = Join-Path $SourceRoot "out/libinochi2d.so"
    $StagedRelative = "lib/libinochi2d.so"
    $SymbolTool = $AndroidNm
    $SymbolArguments = @("--defined-only", "--dynamic")
    $NdkRevision = Get-Content -LiteralPath $PropertiesPath |
        Select-String -Pattern '^Pkg\.Revision\s*='
    if (-not $NdkRevision) { throw "Could not identify the checked Android NDK" }
    $SdkIdentity = "Android NDK $($NdkRevision.Line.Split('=')[1].Trim()); API 26"
}

Push-Location $SourceRoot
try {
    & $Dub.Source @BuildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Inochi2D dynamic C FFI source build failed for $Target"
    }
}
finally {
    Pop-Location
}
if (-not (Test-Path -LiteralPath $BuiltLibrary -PathType Leaf)) {
    throw "Inochi2D build did not produce the expected runtime: $BuiltLibrary"
}

$ResolvedSelections =
    Get-Content -LiteralPath $SelectionsPath -Raw -Encoding utf8 |
    ConvertFrom-Json
if ($ResolvedSelections.fileVersion -ne 1 -or
    @($ResolvedSelections.versions.PSObject.Properties).Count -ne
        $PinnedDependencies.Count) {
    throw "DUB resolved an unapproved Inochi2D dependency set"
}
foreach ($Name in $PinnedDependencies.Keys) {
    if ($ResolvedSelections.versions.$Name -ne $PinnedDependencies[$Name]) {
        throw "DUB changed pinned dependency $Name"
    }
}

$ExportOutput = (& $SymbolTool @SymbolArguments $BuiltLibrary) -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect Inochi2D runtime exports"
}
foreach ($Symbol in $RequiredSymbols) {
    if ($ExportOutput -notmatch "(?m)\b$([regex]::Escape($Symbol))\b") {
        throw "Built Inochi2D runtime is missing required export: $Symbol"
    }
}

if (Test-Path -LiteralPath $InstallRoot) {
    Assert-StrictChildPath $BuildRoot $InstallRoot "Inochi2D install root"
    Remove-Item -LiteralPath $InstallRoot -Recurse -Force
}
$StagedLibrary = Join-Path $InstallRoot $StagedRelative
New-Item -ItemType Directory -Force -Path `
    (Split-Path -Parent $StagedLibrary) | Out-Null
Copy-Item -LiteralPath $BuiltLibrary -Destination $StagedLibrary
Copy-Item -LiteralPath $LicensePath -Destination (Join-Path $InstallRoot "LICENSE")

# Preserve dependency notices verbatim. Metadata is UTF-8, while each license
# body is appended as the exact byte sequence from its verified package archive.
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$ThirdPartyNoticeStream = [System.IO.MemoryStream]::new()
function Write-ThirdPartyNoticeText {
    param([string]$Text)
    $Bytes = $Utf8NoBom.GetBytes($Text)
    $ThirdPartyNoticeStream.Write($Bytes, 0, $Bytes.Length)
}
try {
    Write-ThirdPartyNoticeText `
        "Creator Studio Inochi2D runtime third-party notices`n"
    Write-ThirdPartyNoticeText `
        "License bodies below are verbatim from hash-verified archives.`n`n"
    foreach ($Name in $PinnedDependencies.Keys) {
        $Version = $PinnedDependencies[$Name]
        $PackageLicensePath = Join-Path $DubHome `
            "packages/$Name/$Version/$Name/$($DependencyLicenseFiles[$Name])"
        if (-not (Test-Path -LiteralPath $PackageLicensePath -PathType Leaf)) {
            throw "Verified DUB package license is missing: $Name $Version"
        }
        Write-ThirdPartyNoticeText `
            "================================================================================`n"
        Write-ThirdPartyNoticeText "Component: $Name`n"
        Write-ThirdPartyNoticeText "Version: $Version`n"
        Write-ThirdPartyNoticeText `
            "Source archive: https://code.dlang.org/packages/$Name/$Version.zip`n"
        Write-ThirdPartyNoticeText `
            "Archive SHA-256: $($PinnedDependencyHashes[$Name])`n"
        Write-ThirdPartyNoticeText `
            "License: $($DependencyLicenses[$Name])`n"
        Write-ThirdPartyNoticeText `
            "License file: $($DependencyLicenseFiles[$Name])`n"
        Write-ThirdPartyNoticeText `
            "--------------------------------------------------------------------------------`n"
        [byte[]]$LicenseBytes =
            [System.IO.File]::ReadAllBytes($PackageLicensePath)
        $ThirdPartyNoticeStream.Write(
            $LicenseBytes, 0, $LicenseBytes.Length)
        if ($LicenseBytes.Length -eq 0 -or
            $LicenseBytes[$LicenseBytes.Length - 1] -ne 0x0a) {
            $ThirdPartyNoticeStream.WriteByte(0x0a)
        }
        Write-ThirdPartyNoticeText "`n"
    }
    $ThirdPartyNoticesPath =
        Join-Path $InstallRoot "THIRD_PARTY_NOTICES.txt"
    [System.IO.File]::WriteAllBytes(
        $ThirdPartyNoticesPath, $ThirdPartyNoticeStream.ToArray())
}
finally {
    $ThirdPartyNoticeStream.Dispose()
}
$LibraryHash = (Get-FileHash -LiteralPath $StagedLibrary `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$ThirdPartyNoticesHash = (Get-FileHash -LiteralPath $ThirdPartyNoticesPath `
    -Algorithm SHA256).Hash.ToLowerInvariant()

$Manifest = [ordered]@{
    schema_version = 1
    component = "Inochi2D C-FFI"
    version = $ExpectedVersion
    source_commit = $ExpectedSourceCommit
    source_archive_sha256 = $ExpectedSourceArchiveSha256
    license = "BSD-2-Clause"
    linking = "dynamic"
    dependencies = [ordered]@{}
    target = $Target
    target_triple = $TargetTriple
    minimum_platform = $MinimumPlatform
    compiler = "$CompilerIdentity; $DubVersion"
    sdk = $SdkIdentity
    abi_mode = "IN_VEC2_POSITION"
    library = [ordered]@{
        path = $StagedRelative
        sha256 = $LibraryHash
    }
    notice = [ordered]@{
        path = "LICENSE"
        sha256 = $ExpectedLicenseSha256
    }
    third_party_notices = [ordered]@{
        path = "THIRD_PARTY_NOTICES.txt"
        sha256 = $ThirdPartyNoticesHash
    }
    symbols = $RequiredSymbols
}
foreach ($Name in $PinnedDependencies.Keys) {
    $Manifest.dependencies[$Name] = [ordered]@{
        version = $PinnedDependencies[$Name]
        archive_sha256 = $PinnedDependencyHashes[$Name]
    }
}
$ManifestPath = Join-Path $InstallRoot "runtime-manifest.json"
[System.IO.File]::WriteAllText(
    $ManifestPath, ($Manifest | ConvertTo-Json -Depth 6),
    [System.Text.UTF8Encoding]::new($false))

& (Join-Path $PSScriptRoot "verify_inochi2d_runtime.ps1") `
    -RuntimeRoot $InstallRoot -ExpectedTarget $Target
if ($LASTEXITCODE -ne 0) {
    throw "Generated Inochi2D runtime did not pass verification"
}
Write-Host "Audited Inochi2D root: $InstallRoot"
Write-Host "CS_INOCHI2D_ROOT=$InstallRoot"
