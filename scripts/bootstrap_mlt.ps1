[CmdletBinding()]
param(
    [string]$VcpkgRoot = "",
    [string]$DependencyInstallRoot = "",
    [string]$FfmpegRoot = "",
    [string]$BuildRoot = "",
    [string]$InstallRoot = "",
    [string]$OfficialArchivePath = "",
    [ValidateSet("x64-windows")]
    [string]$Triplet = "x64-windows"
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$PinnedVcpkgCommit = "43643e1f5cf73db40d0d4bd610183348eb09b24e"
$ExpectedMltVersion = "7.40.0"
$ExpectedSourceCommit = "bef9d89c0c279e558d9625dac3399c2aa3d961bc"
$ExpectedSourceArchiveSha256 = "49070c3aa84af719de77875d44a62a1c115aff923aff60657fe6dbaaef877601"
$ExpectedFfmpegVersion = "8.1.2"
$ExpectedFfmpegArchiveSha256 = "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
$OfficialSourceArchiveUrl = "https://github.com/mltframework/mlt/archive/refs/tags/v$ExpectedMltVersion.tar.gz"

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $RepositoryRoot "build/tools/vcpkg"
}
if ([string]::IsNullOrWhiteSpace($DependencyInstallRoot)) {
    $DependencyInstallRoot = Join-Path $RepositoryRoot "build/mlt/vcpkg_installed"
}
if ([string]::IsNullOrWhiteSpace($FfmpegRoot)) {
    $FfmpegRoot = Join-Path $RepositoryRoot "build/ffmpeg/vcpkg_installed/$Triplet"
}
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepositoryRoot "build/mlt"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $BuildRoot "prefix"
}
if ([string]::IsNullOrWhiteSpace($OfficialArchivePath)) {
    $OfficialArchivePath = Join-Path $RepositoryRoot "build/downloads/mlt-v$ExpectedMltVersion.tar.gz"
}

$VcpkgRoot = [System.IO.Path]::GetFullPath($VcpkgRoot)
$DependencyInstallRoot = [System.IO.Path]::GetFullPath($DependencyInstallRoot)
$FfmpegRoot = [System.IO.Path]::GetFullPath($FfmpegRoot)
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$OfficialArchivePath = [System.IO.Path]::GetFullPath($OfficialArchivePath)
$SourceRoot = Join-Path $BuildRoot "source"
$NativeBuildRoot = Join-Path $BuildRoot "native-build"

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    -not (Get-Command cmake.exe -ErrorAction SilentlyContinue) -or
    -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    $VsDevCmd = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $VsDevCmd)) {
        throw "Visual Studio 2022 x64 build environment was not found"
    }
    $EnvironmentLines = & cmd.exe /d /s /c "`"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "Could not initialize the MSVC x64 environment" }
    foreach ($Line in $EnvironmentLines) {
        $Separator = $Line.IndexOf('=')
        if ($Separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $Line.Substring(0, $Separator), $Line.Substring($Separator + 1),
                [EnvironmentVariableTarget]::Process)
        }
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $FfmpegRoot "creator-studio-ffmpeg-build.txt"))) {
    throw "Audited FFmpeg root is required before MLT: $FfmpegRoot"
}
$FfmpegEvidence = Get-Content -LiteralPath (Join-Path $FfmpegRoot "creator-studio-ffmpeg-build.txt") -Raw -Encoding utf8
if ($FfmpegEvidence -match '--enable-(gpl|nonfree)' -or $FfmpegEvidence -notmatch '--enable-shared') {
    throw "FFmpeg root does not satisfy the dynamic LGPL policy"
}
if ($FfmpegEvidence -notmatch "(?m)^ffmpeg_version=$ExpectedFfmpegVersion$" -or
    $FfmpegEvidence -notmatch "(?m)^official_release_archive_sha256=$ExpectedFfmpegArchiveSha256$" -or
    $FfmpegEvidence -notmatch "(?m)^vcpkg_commit=$PinnedVcpkgCommit$") {
    throw "FFmpeg root identity does not match the audited release"
}

if (-not (Test-Path -LiteralPath $OfficialArchivePath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OfficialArchivePath) | Out-Null
    $DownloadPath = "$OfficialArchivePath.download"
    Invoke-WebRequest -Uri $OfficialSourceArchiveUrl -OutFile $DownloadPath
    Move-Item -LiteralPath $DownloadPath -Destination $OfficialArchivePath
}
$ArchiveHash = (Get-FileHash -LiteralPath $OfficialArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $ExpectedSourceArchiveSha256) {
    throw "Official MLT source archive hash mismatch: $OfficialArchivePath"
}

if ((Test-Path -LiteralPath $VcpkgRoot) -and -not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git"))) {
    throw "Refusing to replace non-vcpkg directory: $VcpkgRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $VcpkgRoot) | Out-Null
    & git init $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw "git init failed" }
    & git -C $VcpkgRoot remote add origin https://github.com/microsoft/vcpkg.git
}
& git -C $VcpkgRoot fetch --depth 1 origin $PinnedVcpkgCommit
if ($LASTEXITCODE -ne 0) { throw "Could not fetch pinned vcpkg commit" }
& git -C $VcpkgRoot checkout --detach $PinnedVcpkgCommit
if ($LASTEXITCODE -ne 0) { throw "Could not checkout pinned vcpkg commit" }
& (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }

$Vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
$BuildDependencies = @(
    "pthreads:$Triplet",
    "dirent:$Triplet",
    "libiconv:$Triplet",
    "dlfcn-win32:$Triplet",
    "pkgconf:$Triplet"
)
& $Vcpkg install @BuildDependencies "--x-install-root=$DependencyInstallRoot" --clean-after-build
if ($LASTEXITCODE -ne 0) { throw "MLT build dependency installation failed" }

if (Test-Path -LiteralPath $SourceRoot) {
    $ResolvedSource = [System.IO.Path]::GetFullPath($SourceRoot)
    if (-not $ResolvedSource.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove source directory outside MLT build root"
    }
    Remove-Item -LiteralPath $ResolvedSource -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $SourceRoot | Out-Null
tar -xf $OfficialArchivePath -C $SourceRoot --strip-components=1
if ($LASTEXITCODE -ne 0) { throw "Could not extract pinned MLT source" }

$CMakeLists = Get-Content -LiteralPath (Join-Path $SourceRoot "CMakeLists.txt") -Raw -Encoding utf8
if ($CMakeLists -notmatch 'project\(MLT\s+VERSION\s+7\.40\.0') {
    throw "Extracted MLT source version is not $ExpectedMltVersion"
}

if (Test-Path -LiteralPath $NativeBuildRoot) {
    $ResolvedNativeBuild = [System.IO.Path]::GetFullPath($NativeBuildRoot)
    if (-not $ResolvedNativeBuild.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                              [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove native build directory outside MLT build root"
    }
    Remove-Item -LiteralPath $ResolvedNativeBuild -Recurse -Force
}

$DependencyPrefix = Join-Path $DependencyInstallRoot $Triplet
$VcpkgToolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
$ConfigureFlags = @(
    "-S", $SourceRoot,
    "-B", $NativeBuildRoot,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_FLAGS=/utf-8",
    "-DCMAKE_CXX_FLAGS=/utf-8",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DVCPKG_INSTALLED_DIR=$DependencyInstallRoot",
    "-DVCPKG_MANIFEST_MODE=OFF",
    "-DCMAKE_PREFIX_PATH=$FfmpegRoot;$DependencyPrefix",
    "-DFFmpeg_ROOT=$FfmpegRoot",
    "-DGPL=OFF",
    "-DGPL3=OFF",
    "-DBUILD_TESTING=OFF",
    "-DBUILD_DOCS=OFF",
    "-DCLANG_FORMAT=OFF",
    "-DMOD_AVFORMAT=ON",
    "-DUSE_AVDEVICE=OFF",
    "-DUSE_LV2=OFF",
    "-DUSE_VST2=OFF",
    "-DMOD_DECKLINK=OFF",
    "-DMOD_FREI0R=OFF",
    "-DMOD_GDK=OFF",
    "-DMOD_GLAXNIMATE_QT6=OFF",
    "-DMOD_JACKRACK=OFF",
    "-DMOD_KDENLIVE=OFF",
    "-DMOD_MOVIT=OFF",
    "-DMOD_NDI=OFF",
    "-DMOD_NORMALIZE=OFF",
    "-DMOD_OLDFILM=OFF",
    "-DMOD_OPENCV=OFF",
    "-DMOD_OPENFX=OFF",
    "-DMOD_PLUS=OFF",
    "-DMOD_PLUSGPL=OFF",
    "-DMOD_QT6=OFF",
    "-DMOD_RESAMPLE=OFF",
    "-DMOD_RTAUDIO=OFF",
    "-DMOD_RUBBERBAND=OFF",
    "-DMOD_RNNOISE=OFF",
    "-DMOD_SDL1=OFF",
    "-DMOD_SDL2=OFF",
    "-DMOD_SOX=OFF",
    "-DMOD_SPATIALAUDIO=OFF",
    "-DMOD_VIDSTAB=OFF",
    "-DMOD_VORBIS=OFF",
    "-DMOD_XINE=OFF",
    "-DMOD_XML=OFF",
    "-DSWIG_CSHARP=OFF",
    "-DSWIG_JAVA=OFF",
    "-DSWIG_LUA=OFF",
    "-DSWIG_NODEJS=OFF",
    "-DSWIG_PERL=OFF",
    "-DSWIG_PHP=OFF",
    "-DSWIG_PYTHON=OFF",
    "-DSWIG_RUBY=OFF",
    "-DSWIG_TCL=OFF"
)
& cmake @ConfigureFlags
if ($LASTEXITCODE -ne 0) { throw "MLT configure failed" }
& cmake --build $NativeBuildRoot --config Release --target mlt mlt++ mltcore mltavformat
if ($LASTEXITCODE -ne 0) { throw "Selected MLT target build failed" }

if (Test-Path -LiteralPath $InstallRoot) {
    $ResolvedInstall = [System.IO.Path]::GetFullPath($InstallRoot)
    if (-not $ResolvedInstall.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                         [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace install directory outside MLT build root"
    }
    Remove-Item -LiteralPath $ResolvedInstall -Recurse -Force
}
foreach ($Directory in @("bin", "lib/mlt-7", "include/mlt-7/framework", "include/mlt-7/mlt++", "include/mlt-deps", "share/mlt-7/core", "share/mlt-7/avformat", "share/mlt-7/profiles", "share/mlt-7/presets")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $InstallRoot $Directory) | Out-Null
}

function Copy-RequiredFile {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required selected MLT artifact is missing: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}
function Copy-SelectedTree {
    param([string]$Source, [string]$Destination, [string[]]$Patterns)
    foreach ($Pattern in $Patterns) {
        Get-ChildItem -LiteralPath $Source -File -Filter $Pattern | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
        }
    }
}
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

$MltOutput = Join-Path $NativeBuildRoot "out"
Copy-RequiredFile (Join-Path $MltOutput "mlt-7.dll") (Join-Path $InstallRoot "bin/mlt-7.dll")
Copy-RequiredFile (Join-Path $MltOutput "mlt++-7.dll") (Join-Path $InstallRoot "bin/mlt++-7.dll")
Copy-RequiredFile (Join-Path $MltOutput "lib/mlt/mltcore.dll") (Join-Path $InstallRoot "lib/mlt-7/mltcore.dll")
Copy-RequiredFile (Join-Path $MltOutput "lib/mlt/mltavformat.dll") (Join-Path $InstallRoot "lib/mlt-7/mltavformat.dll")
Copy-RequiredFile (Join-Path $MltOutput "lib/mlt-7.lib") (Join-Path $InstallRoot "lib/mlt-7.lib")
Copy-RequiredFile (Join-Path $MltOutput "lib/mlt++-7.lib") (Join-Path $InstallRoot "lib/mlt++-7.lib")

Copy-SelectedTree (Join-Path $SourceRoot "src/framework") (Join-Path $InstallRoot "include/mlt-7/framework") @("*.h")
Copy-RequiredFile (Join-Path $NativeBuildRoot "src/framework/mlt_export.h") (Join-Path $InstallRoot "include/mlt-7/framework/mlt_export.h")
Copy-SelectedTree (Join-Path $SourceRoot "src/mlt++") (Join-Path $InstallRoot "include/mlt-7/mlt++") @("*.h")
foreach ($Header in @("pthread.h", "sched.h", "_ptw32.h")) {
    Copy-RequiredFile (Join-Path $DependencyPrefix "include/$Header") (Join-Path $InstallRoot "include/mlt-deps/$Header")
}
Copy-SelectedTree (Join-Path $SourceRoot "src/modules/core") (Join-Path $InstallRoot "share/mlt-7/core") @("*.yml", "*.ini", "*.dict")
Copy-SelectedTree (Join-Path $SourceRoot "src/modules/avformat") (Join-Path $InstallRoot "share/mlt-7/avformat") @("*.yml", "*.txt")
Copy-SelectedTree (Join-Path $SourceRoot "profiles") (Join-Path $InstallRoot "share/mlt-7/profiles") @("*")
Copy-SelectedTree (Join-Path $SourceRoot "presets") (Join-Path $InstallRoot "share/mlt-7/presets") @("*")

$RuntimeDllPatterns = @("avcodec-*.dll", "avfilter-*.dll", "avformat-*.dll", "avutil-*.dll", "swresample-*.dll", "swscale-*.dll", "pthread*.dll", "iconv*.dll", "libiconv*.dll", "dl*.dll")
foreach ($Prefix in @($FfmpegRoot, $DependencyPrefix)) {
    $Bin = Join-Path $Prefix "bin"
    if (Test-Path -LiteralPath $Bin) {
        Copy-SelectedTree $Bin (Join-Path $InstallRoot "bin") $RuntimeDllPatterns
    }
}

$EvidencePath = Join-Path $InstallRoot "creator-studio-mlt-build.txt"
$Evidence = @(
    "Creator Studio MLT build evidence",
    "mlt_version=$ExpectedMltVersion",
    "source_commit=$ExpectedSourceCommit",
    "official_source_archive_sha256=$ExpectedSourceArchiveSha256",
    "verified_official_archive=$OfficialArchivePath",
    "vcpkg_commit=$PinnedVcpkgCommit",
    "triplet=$Triplet",
    "dynamic_linking=true",
    "melt_packaged=false",
    "allowed_modules=core,avformat",
    "",
    ($ConfigureFlags -join [Environment]::NewLine)
) -join [Environment]::NewLine
[System.IO.File]::WriteAllText($EvidencePath, $Evidence, [System.Text.UTF8Encoding]::new($false))

function Get-FileProvenance {
    param([string]$Relative)
    $Lower = $Relative.ToLowerInvariant()
    $Name = [System.IO.Path]::GetFileName($Lower)
    $VcpkgIdentity = "vcpkg:$PinnedVcpkgCommit"
    if ($Name -match '^(avcodec-|avfilter-|avformat-|avutil-|swresample-|swscale-).*\.dll$') {
        return [ordered]@{ component = "FFmpeg"; version = $ExpectedFfmpegVersion; source_identity = "sha256:$ExpectedFfmpegArchiveSha256"; license = "LGPL-2.1-or-later" }
    }
    if ($Name -match '^pthread.*\.dll$' -or $Lower.StartsWith("include/mlt-deps/")) {
        return [ordered]@{ component = "PThreads4W"; version = "3.0.0"; source_identity = $VcpkgIdentity; license = "Apache-2.0" }
    }
    if ($Name -in @("iconv-2.dll", "libiconv-2.dll")) {
        return [ordered]@{ component = "GNU libiconv"; version = "1.19"; source_identity = $VcpkgIdentity; license = "LGPL-2.1-or-later" }
    }
    if ($Name -eq "dl.dll") {
        return [ordered]@{ component = "dlfcn-win32"; version = "1.4.2"; source_identity = $VcpkgIdentity; license = "MIT" }
    }
    if ($Lower -eq "creator-studio-mlt-build.txt") {
        return [ordered]@{ component = "Creator Studio"; version = "1"; source_identity = "repository:R1-03"; license = "LicenseRef-Creator-Studio-Proprietary" }
    }
    if ($Lower.StartsWith("bin/mlt") -or $Lower.StartsWith("lib/") -or
        $Lower.StartsWith("share/") -or $Lower.StartsWith("include/mlt-7/")) {
        return [ordered]@{ component = "MLT Framework"; version = $ExpectedMltVersion; source_identity = $ExpectedSourceCommit; license = "LGPL-2.1-or-later" }
    }
    throw "No approved provenance classification for MLT artifact: $Relative"
}

$ManifestFiles = @()
foreach ($File in Get-ChildItem -LiteralPath $InstallRoot -Recurse -File | Sort-Object FullName) {
    $Relative = (Get-CompatibleRelativePath $InstallRoot $File.FullName).Replace('\', '/')
    $Role = if ($Relative.StartsWith("bin/")) { "runtime-library" } elseif ($Relative.StartsWith("lib/mlt-7/")) { "runtime-module" } elseif ($Relative.StartsWith("share/")) { "runtime-data" } elseif ($Relative.StartsWith("include/") -or $Relative.EndsWith(".lib")) { "development" } else { "evidence" }
    $Provenance = Get-FileProvenance $Relative
    $ManifestFiles += [ordered]@{
        path = $Relative
        sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        role = $Role
        component = $Provenance.component
        version = $Provenance.version
        source_identity = $Provenance.source_identity
        license = $Provenance.license
    }
}
$Manifest = [ordered]@{
    abi = 1
    component = "MLT Framework"
    version = $ExpectedMltVersion
    source_commit = $ExpectedSourceCommit
    linking = "dynamic"
    allowed_modules = @("core", "avformat")
    dependencies = @(
        [ordered]@{ component = "FFmpeg"; version = $ExpectedFfmpegVersion; source_identity = "sha256:$ExpectedFfmpegArchiveSha256"; license = "LGPL-2.1-or-later" }
        [ordered]@{ component = "PThreads4W"; version = "3.0.0"; source_identity = "vcpkg:$PinnedVcpkgCommit"; license = "Apache-2.0" }
        [ordered]@{ component = "GNU libiconv"; version = "1.19"; source_identity = "vcpkg:$PinnedVcpkgCommit"; license = "LGPL-2.1-or-later" }
        [ordered]@{ component = "dlfcn-win32"; version = "1.4.2"; source_identity = "vcpkg:$PinnedVcpkgCommit"; license = "MIT" }
    )
    files = $ManifestFiles
}
$ManifestPath = Join-Path $InstallRoot "mlt-runtime-manifest.json"
[System.IO.File]::WriteAllText($ManifestPath, ($Manifest | ConvertTo-Json -Depth 6), [System.Text.UTF8Encoding]::new($false))

& (Join-Path $PSScriptRoot "verify_mlt_runtime.ps1") -RuntimeRoot $InstallRoot -ManifestPath $ManifestPath
if ($LASTEXITCODE -ne 0) { throw "Generated MLT runtime did not pass verification" }

Write-Host "Audited MLT root: $InstallRoot"
Write-Host "Build evidence: $EvidencePath"
Write-Host "Runtime manifest: $ManifestPath"
