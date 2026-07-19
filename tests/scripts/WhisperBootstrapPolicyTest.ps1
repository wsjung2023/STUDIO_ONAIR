[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

# Fail-closed policy test for the audited whisper.cpp bootstrap, registered as the
# `WhisperBootstrapPolicy` ctest (mirrors MltBootstrapPolicyTest). It asserts the
# bootstrap script pins an exact release tag + source commit, verifies a REAL
# SHA-256 for BOTH the source archive and the pinned ggml model, records its
# configure flags, and contains no forbidden (GPL / GPU / non-free / auto-model-
# download) build option. It also checks the gate wiring stays default-OFF and the
# adapter identity constants match the script.

$ErrorActionPreference = "Stop"
$BootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_whisper.ps1"
$ManifestHeader = Join-Path $RepositoryRoot "src/whisper_adapter/WhisperRuntimeManifest.cpp"
$AdapterCMake = Join-Path $RepositoryRoot "src/whisper_adapter/CMakeLists.txt"

if (-not (Test-Path -LiteralPath $BootstrapPath)) {
    throw "Missing whisper bootstrap: $BootstrapPath"
}
if (-not (Test-Path -LiteralPath $ManifestHeader)) {
    throw "Missing whisper runtime manifest verifier: $ManifestHeader"
}
if (-not (Test-Path -LiteralPath $AdapterCMake)) {
    throw "Missing whisper adapter CMake: $AdapterCMake"
}

$Bootstrap = Get-Content -LiteralPath $BootstrapPath -Raw -Encoding utf8
$RequiredPatterns = @(
    '1\.7\.6',                                                             # pinned release tag/version
    'a8d002cfd879315632a579e73f0148d06959de36',                           # pinned source commit
    '166140e9a6d8a36f787a2bd77f8f44dd64874f12dd8359ff7c1f4f9acb86202e',   # real source archive SHA-256
    '921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f',   # real ggml model SHA-256
    'ggml-tiny\.en\.bin',
    'ExpectedSourceArchiveSha256',
    'ExpectedModelSha256',
    'Get-FileHash',
    'archive/refs/tags/v\$ExpectedWhisperVersion\.tar\.gz',               # official source URL
    'huggingface\.co/ggerganov/whisper\.cpp',                             # official model URL
    'BUILD_SHARED_LIBS=ON',
    'WHISPER_BUILD_TESTS=OFF',
    'WHISPER_CURL=OFF',
    'GGML_CUDA=OFF',
    'whisper-runtime-manifest\.json',
    'creator-studio-whisper-build\.txt'
)
foreach ($Pattern in $RequiredPatterns) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "whisper bootstrap is missing required policy evidence: $Pattern"
    }
}

# Forbidden build options: nothing GPL/non-free, no GPU backend, no runtime model
# auto-download. whisper.cpp + ggml + the OpenAI weights are all MIT. The `-D`
# prefix is required so this does not trip on the bootstrap's own defensive guard
# regex, which lists the ON forms (without `-D`) precisely in order to reject them.
$ForbiddenPatterns = @(
    '-DWHISPER_CURL=ON',
    '-DGGML_CUDA=ON',
    '-DGGML_HIP=ON',
    '-DGGML_VULKAN=ON',
    '-DGGML_SYCL=ON',
    '-DWHISPER_COREML=ON',
    '-DWHISPER_OPENVINO=ON',
    '-DGPL=ON',
    '-DGPL3=ON'
)
foreach ($Pattern in $ForbiddenPatterns) {
    if ($Bootstrap -match $Pattern) {
        throw "whisper bootstrap contains a forbidden build option: $Pattern"
    }
}

# The bootstrap must actually verify hashes (fail-closed on mismatch), not just
# mention them.
foreach ($Pattern in @(
    'if \(\$ArchiveHash -ne \$ExpectedSourceArchiveSha256\)',
    'if \(\$ModelHash -ne \$ExpectedModelSha256\)'
)) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "whisper bootstrap does not fail closed on a hash mismatch: $Pattern"
    }
}

# The C++ runtime verifier must re-check identity + reject GPL before load.
$Manifest = Get-Content -LiteralPath $ManifestHeader -Raw -Encoding utf8
foreach ($Pattern in @('CS_WHISPER_EXPECTED_MODEL_SHA256', 'sha256File', 'GPL', 'whisper-runtime-manifest\.json')) {
    if ($Manifest -notmatch $Pattern) {
        throw "whisper runtime verifier is missing a fail-closed check: $Pattern"
    }
}

# Adapter identity constants must match the bootstrap pins.
$Cmake = Get-Content -LiteralPath $AdapterCMake -Raw -Encoding utf8
foreach ($Pattern in @(
    'CS_WHISPER_EXPECTED_VERSION="1\.7\.6"',
    'CS_WHISPER_EXPECTED_COMMIT="a8d002cfd879315632a579e73f0148d06959de36"',
    'CS_WHISPER_EXPECTED_MODEL_SHA256="921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f"'
)) {
    if ($Cmake -notmatch $Pattern) {
        throw "whisper adapter identity constant does not match the bootstrap pin: $Pattern"
    }
}

# The gate must default OFF and require CS_WHISPER_ROOT when enabled.
$RootCMake = Get-Content -LiteralPath (Join-Path $RepositoryRoot "CMakeLists.txt") -Raw -Encoding utf8
if ($RootCMake -notmatch 'option\(CS_ENABLE_WHISPER "[^"]*" OFF\)') {
    throw "CS_ENABLE_WHISPER must be declared as a default-OFF option"
}
if ($RootCMake -notmatch 'CS_ENABLE_WHISPER requires CS_WHISPER_ROOT') {
    throw "CS_ENABLE_WHISPER must FATAL_ERROR without CS_WHISPER_ROOT"
}

Write-Host "whisper bootstrap policy is fail-closed."
