[CmdletBinding()]
param(
    [string]$BuildRoot = "",
    [string]$InstallRoot = "",
    [string]$OfficialArchivePath = "",
    [string]$ModelPath = "",
    [ValidateSet("x64-windows")]
    [string]$Triplet = "x64-windows"
)

# Audited-native-dependency bootstrap for the whisper.cpp speech-to-text engine.
#
# Mirrors scripts/bootstrap_ffmpeg.ps1 / bootstrap_mlt.ps1: it pins an exact
# upstream release, downloads the OFFICIAL source archive, verifies its SHA-256
# before trusting a byte of it, builds the library from that verified source with
# a recorded, forbidden-flag-free configuration, downloads a pinned ggml model
# and verifies ITS SHA-256 (ARCHITECTURE.md 11 requires model-download hash
# verification), and emits an audited install prefix as CS_WHISPER_ROOT plus a
# runtime manifest the C++ WhisperRuntimeManifest re-checks before load.
#
# whisper.cpp is MIT and the OpenAI Whisper ggml weights are MIT (see
# legal/OSS_BOM.csv and docs/R2-engine-licensing.md); no GPL, no --enable-gpl
# analogue, nothing non-free is pulled in. GPU/CoreML/OpenVINO/CURL are all kept
# OFF so the build is a reproducible CPU-only, no-network-at-runtime artifact.

$ErrorActionPreference = "Stop"
$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# --- Pins (kept in lock-step with src/whisper_adapter compile definitions and
# --- legal/OSS_BOM.csv). Do not bump without re-verifying the SHA-256 values. ---
$ExpectedWhisperVersion = "1.7.6"
$ExpectedSourceCommit = "a8d002cfd879315632a579e73f0148d06959de36"
$ExpectedSourceArchiveSha256 = "166140e9a6d8a36f787a2bd77f8f44dd64874f12dd8359ff7c1f4f9acb86202e"
$OfficialSourceArchiveUrl = "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v$ExpectedWhisperVersion.tar.gz"

# Pinned ggml model. tiny.en is the smallest English model; its SHA-256 is the
# real HuggingFace git-LFS object hash, confirmed by downloading the file.
$ExpectedModelName = "ggml-tiny.en.bin"
$ExpectedModelSha256 = "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f"
$OfficialModelUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$ExpectedModelName"
$ModelInstallRelative = "share/whisper-models/$ExpectedModelName"
$SampleInstallRelative = "share/whisper-samples/jfk.wav"

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepositoryRoot "build/whisper"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $BuildRoot "prefix"
}
if ([string]::IsNullOrWhiteSpace($OfficialArchivePath)) {
    $OfficialArchivePath = Join-Path $RepositoryRoot "build/downloads/whisper-v$ExpectedWhisperVersion.tar.gz"
}
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $RepositoryRoot "build/downloads/$ExpectedModelName"
}

$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$OfficialArchivePath = [System.IO.Path]::GetFullPath($OfficialArchivePath)
$ModelPath = [System.IO.Path]::GetFullPath($ModelPath)
$SourceRoot = Join-Path $BuildRoot "source"
$NativeBuildRoot = Join-Path $BuildRoot "native-build"

# --- MSVC x64 environment (mirrors bootstrap_mlt.ps1) ---
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

# --- Step 1: download + verify the OFFICIAL source archive BEFORE trusting it ---
if (-not (Test-Path -LiteralPath $OfficialArchivePath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OfficialArchivePath) | Out-Null
    $DownloadPath = "$OfficialArchivePath.download"
    Invoke-WebRequest -Uri $OfficialSourceArchiveUrl -OutFile $DownloadPath
    Move-Item -LiteralPath $DownloadPath -Destination $OfficialArchivePath
}
$ArchiveHash = (Get-FileHash -LiteralPath $OfficialArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $ExpectedSourceArchiveSha256) {
    throw "Official whisper.cpp source archive hash mismatch: $OfficialArchivePath (got $ArchiveHash)"
}

# --- Step 2: extract into a build-root-scoped directory ---
if (Test-Path -LiteralPath $SourceRoot) {
    $ResolvedSource = [System.IO.Path]::GetFullPath($SourceRoot)
    if (-not $ResolvedSource.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove source directory outside whisper build root"
    }
    Remove-Item -LiteralPath $ResolvedSource -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $SourceRoot | Out-Null
tar -xf $OfficialArchivePath -C $SourceRoot --strip-components=1
if ($LASTEXITCODE -ne 0) { throw "Could not extract pinned whisper.cpp source" }

$CMakeLists = Get-Content -LiteralPath (Join-Path $SourceRoot "CMakeLists.txt") -Raw -Encoding utf8
if ($CMakeLists -notmatch 'project\("whisper\.cpp"\s+VERSION\s+1\.7\.6') {
    throw "Extracted whisper.cpp source version is not $ExpectedWhisperVersion"
}

# --- Step 3: configure with a recorded, forbidden-flag-free configuration ---
if (Test-Path -LiteralPath $NativeBuildRoot) {
    $ResolvedNativeBuild = [System.IO.Path]::GetFullPath($NativeBuildRoot)
    if (-not $ResolvedNativeBuild.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                              [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove native build directory outside whisper build root"
    }
    Remove-Item -LiteralPath $ResolvedNativeBuild -Recurse -Force
}

$ConfigureFlags = @(
    "-S", $SourceRoot,
    "-B", $NativeBuildRoot,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_INSTALL_PREFIX=$InstallRoot",
    "-DBUILD_SHARED_LIBS=ON",
    "-DWHISPER_BUILD_TESTS=OFF",
    "-DWHISPER_BUILD_EXAMPLES=OFF",
    "-DWHISPER_BUILD_SERVER=OFF",
    "-DWHISPER_CURL=OFF",
    "-DWHISPER_SDL2=OFF",
    "-DWHISPER_FFMPEG=OFF",
    "-DWHISPER_COREML=OFF",
    "-DWHISPER_OPENVINO=OFF",
    "-DWHISPER_FATAL_WARNINGS=OFF",
    "-DGGML_CUDA=OFF",
    "-DGGML_HIP=OFF",
    "-DGGML_VULKAN=OFF",
    "-DGGML_SYCL=OFF",
    "-DGGML_METAL=OFF",
    "-DGGML_OPENCL=OFF"
)

# Guard: refuse any forbidden build option even if a caller edited the flag list.
foreach ($Flag in $ConfigureFlags) {
    if ($Flag -match '(?i)(WHISPER_CURL=ON|GGML_CUDA=ON|GGML_HIP=ON|GGML_VULKAN=ON|GGML_SYCL=ON|WHISPER_COREML=ON|WHISPER_OPENVINO=ON)') {
        throw "Forbidden whisper.cpp build option requested: $Flag"
    }
}

& cmake @ConfigureFlags
if ($LASTEXITCODE -ne 0) { throw "whisper.cpp configure failed" }
& cmake --build $NativeBuildRoot --config Release --target whisper
if ($LASTEXITCODE -ne 0) { throw "whisper.cpp build failed" }

if (Test-Path -LiteralPath $InstallRoot) {
    $ResolvedInstall = [System.IO.Path]::GetFullPath($InstallRoot)
    if (-not $ResolvedInstall.StartsWith($BuildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                         [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace install directory outside whisper build root"
    }
    Remove-Item -LiteralPath $ResolvedInstall -Recurse -Force
}
& cmake --install $NativeBuildRoot --config Release
if ($LASTEXITCODE -ne 0) { throw "whisper.cpp install failed" }

# --- Step 4: download + verify the pinned ggml model, then stage it ---
if (-not (Test-Path -LiteralPath $ModelPath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ModelPath) | Out-Null
    $ModelDownloadPath = "$ModelPath.download"
    Invoke-WebRequest -Uri $OfficialModelUrl -OutFile $ModelDownloadPath
    Move-Item -LiteralPath $ModelDownloadPath -Destination $ModelPath
}
$ModelHash = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ModelHash -ne $ExpectedModelSha256) {
    throw "Pinned ggml model hash mismatch: $ModelPath (got $ModelHash)"
}
$StagedModelPath = Join-Path $InstallRoot $ModelInstallRelative
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $StagedModelPath) | Out-Null
Copy-Item -LiteralPath $ModelPath -Destination $StagedModelPath -Force

# Stage the upstream jfk.wav speech sample (source-tree, MIT) for the enabled
# real-inference test; it is test-only evidence, not an audited runtime artifact.
$SourceSample = Join-Path $SourceRoot "samples/jfk.wav"
if (Test-Path -LiteralPath $SourceSample) {
    $StagedSample = Join-Path $InstallRoot $SampleInstallRelative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $StagedSample) | Out-Null
    Copy-Item -LiteralPath $SourceSample -Destination $StagedSample -Force
}

# --- Step 5: evidence + runtime manifest (re-checked by WhisperRuntimeManifest) ---
$Evidence = @(
    "Creator Studio whisper.cpp build evidence"
    "whisper_version=$ExpectedWhisperVersion"
    "source_commit=$ExpectedSourceCommit"
    "official_source_archive_sha256=$ExpectedSourceArchiveSha256"
    "verified_official_archive=$OfficialArchivePath"
    "model_name=$ExpectedModelName"
    "model_sha256=$ExpectedModelSha256"
    "dynamic_linking=true"
    "gpu=disabled"
    ""
    ($ConfigureFlags -join [Environment]::NewLine)
) -join [Environment]::NewLine
$EvidencePath = Join-Path $InstallRoot "creator-studio-whisper-build.txt"
[System.IO.File]::WriteAllText($EvidencePath, $Evidence, [System.Text.UTF8Encoding]::new($false))

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

function Get-WhisperProvenance {
    param([string]$Relative)
    $Lower = $Relative.ToLowerInvariant()
    $Name = [System.IO.Path]::GetFileName($Lower)
    if ($Lower -eq $ModelInstallRelative.ToLowerInvariant()) {
        return [ordered]@{ component = "OpenAI Whisper (ggml)"; version = $ExpectedModelName; source_identity = "sha256:$ExpectedModelSha256"; license = "MIT" }
    }
    if ($Lower -eq "creator-studio-whisper-build.txt") {
        return [ordered]@{ component = "Creator Studio"; version = "1"; source_identity = "repository:R2-05"; license = "LicenseRef-Creator-Studio-Proprietary" }
    }
    if ($Name -like "ggml*.dll" -or $Name -like "ggml*.lib") {
        return [ordered]@{ component = "ggml"; version = $ExpectedWhisperVersion; source_identity = $ExpectedSourceCommit; license = "MIT" }
    }
    if ($Name -like "whisper*.dll" -or $Name -like "whisper*.lib") {
        return [ordered]@{ component = "whisper.cpp"; version = $ExpectedWhisperVersion; source_identity = $ExpectedSourceCommit; license = "MIT" }
    }
    # Everything else in the audited prefix (headers, cmake config, pkgconfig) is
    # whisper.cpp / ggml MIT source. No GPL, no non-free artifact is ever staged.
    return [ordered]@{ component = "whisper.cpp"; version = $ExpectedWhisperVersion; source_identity = $ExpectedSourceCommit; license = "MIT" }
}

# The runtime library file set whisper's installer emits varies by backend, so
# (unlike the frozen MLT tree) the manifest pins the model exactly and records a
# SHA-256 for every runtime DLL and the model; WhisperRuntimeManifest re-hashes
# each listed file and rejects any GPL/forbidden entry before load.
$ManifestFiles = @()
$ManifestTargets = @()
Get-ChildItem -LiteralPath (Join-Path $InstallRoot "bin") -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -ieq ".dll" } | ForEach-Object { $ManifestTargets += $_.FullName }
$ManifestTargets += $StagedModelPath
$ManifestTargets += $EvidencePath
foreach ($FullName in $ManifestTargets) {
    $Relative = (Get-CompatibleRelativePath $InstallRoot $FullName).Replace('\', '/')
    $Role = if ($Relative -eq $ModelInstallRelative) { "model" }
        elseif ($Relative -like "bin/*") { "runtime-library" }
        else { "evidence" }
    $Provenance = Get-WhisperProvenance $Relative
    if ($Provenance.license -match 'GPL') {
        throw "Refusing to record a GPL artifact in the whisper runtime manifest: $Relative"
    }
    $ManifestFiles += [ordered]@{
        path = $Relative
        sha256 = (Get-FileHash -LiteralPath $FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        role = $Role
        component = $Provenance.component
        version = $Provenance.version
        source_identity = $Provenance.source_identity
        license = $Provenance.license
    }
}

$Manifest = [ordered]@{
    abi = 1
    component = "whisper.cpp"
    version = $ExpectedWhisperVersion
    source_commit = $ExpectedSourceCommit
    linking = "dynamic"
    model = [ordered]@{
        name = $ExpectedModelName
        path = $ModelInstallRelative
        sha256 = $ExpectedModelSha256
        license = "MIT"
        source_url = $OfficialModelUrl
    }
    files = $ManifestFiles
}
$ManifestPath = Join-Path $InstallRoot "whisper-runtime-manifest.json"
[System.IO.File]::WriteAllText($ManifestPath, ($Manifest | ConvertTo-Json -Depth 6), [System.Text.UTF8Encoding]::new($false))

Write-Host "Audited whisper.cpp root: $InstallRoot"
Write-Host "CS_WHISPER_ROOT=$InstallRoot"
Write-Host "Build evidence: $EvidencePath"
Write-Host "Runtime manifest: $ManifestPath"
Write-Host "Verified model: $StagedModelPath ($ExpectedModelName)"
