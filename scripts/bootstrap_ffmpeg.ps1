[CmdletBinding()]
param(
    [string]$VcpkgRoot = "",
    [string]$InstallRoot = "",
    [string]$OfficialArchivePath = "",
    [ValidateSet("x64-windows", "arm64-osx", "x64-osx")]
    [string]$Triplet = "x64-windows"
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$PinnedVcpkgCommit = "43643e1f5cf73db40d0d4bd610183348eb09b24e"
$ExpectedFfmpegVersion = "8.1.2"
$OfficialReleaseArchiveSha256 = "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
$OfficialReleaseArchiveUrl = "https://ffmpeg.org/releases/ffmpeg-$ExpectedFfmpegVersion.tar.xz"
$PackageSpec = "ffmpeg[avcodec,avdevice,avfilter,avformat,ffprobe,swresample,swscale,zlib]:$Triplet"
$RunningOnWindows = $env:OS -eq "Windows_NT"

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $RepositoryRoot "build/tools/vcpkg"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $RepositoryRoot "build/ffmpeg/vcpkg_installed"
}
if ([string]::IsNullOrWhiteSpace($OfficialArchivePath)) {
    $OfficialArchivePath = Join-Path $RepositoryRoot "build/downloads/ffmpeg-$ExpectedFfmpegVersion.tar.xz"
}
$VcpkgRoot = [System.IO.Path]::GetFullPath($VcpkgRoot)
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$OfficialArchivePath = [System.IO.Path]::GetFullPath($OfficialArchivePath)

if (-not (Test-Path -LiteralPath $OfficialArchivePath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OfficialArchivePath) | Out-Null
    $DownloadPath = "$OfficialArchivePath.download"
    Invoke-WebRequest -Uri $OfficialReleaseArchiveUrl -OutFile $DownloadPath
    Move-Item -LiteralPath $DownloadPath -Destination $OfficialArchivePath
}
$ArchiveHash = (Get-FileHash -LiteralPath $OfficialArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $OfficialReleaseArchiveSha256) {
    throw "Official FFmpeg archive hash mismatch: $OfficialArchivePath"
}

if ((Test-Path -LiteralPath $VcpkgRoot) -and
    -not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git"))) {
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

$PortManifestPath = Join-Path $VcpkgRoot "ports/ffmpeg/vcpkg.json"
$PortManifest = Get-Content -LiteralPath $PortManifestPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($PortManifest.version -ne $ExpectedFfmpegVersion) {
    throw "Pinned vcpkg port contains FFmpeg $($PortManifest.version), expected $ExpectedFfmpegVersion"
}
if ($PackageSpec -match "(?i)(all-gpl|all-nonfree|gpl|nonfree|x264|x265|fdk-aac)") {
    throw "Forbidden FFmpeg feature in package specification: $PackageSpec"
}

if ($RunningOnWindows) {
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    $Vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
} else {
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.sh") -disableMetrics
    $Vcpkg = Join-Path $VcpkgRoot "vcpkg"
}
if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }

& $Vcpkg install $PackageSpec "--x-install-root=$InstallRoot" --clean-after-build
if ($LASTEXITCODE -ne 0) { throw "FFmpeg source build failed" }

$Prefix = Join-Path $InstallRoot $Triplet
$ProbeName = if ($RunningOnWindows) { "ffprobe.exe" } else { "ffprobe" }
$Probe = Join-Path $Prefix "tools/ffmpeg/$ProbeName"
if (-not (Test-Path -LiteralPath $Probe)) {
    throw "FFmpeg build completed without the expected capability probe: $Probe"
}

$BuildConfiguration = (& $Probe -buildconf -version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "ffprobe could not report its build configuration" }
if ($BuildConfiguration -match "--enable-(gpl|nonfree)") {
    throw "Forbidden GPL/nonfree FFmpeg configuration detected"
}
if ($BuildConfiguration -notmatch "--enable-shared") {
    throw "FFmpeg must be built as dynamic libraries for the approved LGPL distribution model"
}
if ($BuildConfiguration -notmatch "--enable-zlib") {
    throw "FFmpeg must include zlib for generated PNG overlay decoding"
}
$DecoderList = (& $Probe -v quiet -decoders 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $DecoderList -notmatch "(?m)^\s*V\S*\s+png\s") {
    throw "FFmpeg build completed without the required PNG decoder"
}

$Evidence = @(
    "Creator Studio FFmpeg build evidence"
    "ffmpeg_version=$ExpectedFfmpegVersion"
    "official_release_archive_sha256=$OfficialReleaseArchiveSha256"
    "verified_official_archive=$OfficialArchivePath"
    "source_build_recipe=vcpkg port pinned by vcpkg_commit"
    "vcpkg_commit=$PinnedVcpkgCommit"
    "triplet=$Triplet"
    "package_spec=$PackageSpec"
    "png_decoder=enabled"
    ""
    $BuildConfiguration.Trim()
) -join [Environment]::NewLine
$EvidencePath = Join-Path $Prefix "creator-studio-ffmpeg-build.txt"
[System.IO.File]::WriteAllText($EvidencePath, $Evidence, [System.Text.UTF8Encoding]::new($false))

Write-Host "Audited FFmpeg root: $Prefix"
Write-Host "Build evidence: $EvidencePath"
