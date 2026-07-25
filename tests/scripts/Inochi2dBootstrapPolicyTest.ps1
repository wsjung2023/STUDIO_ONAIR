[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$RepositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "../.."))
$BootstrapPath = Join-Path $RepositoryRoot "scripts/bootstrap_inochi2d.ps1"
$VerifierPath = Join-Path $RepositoryRoot "scripts/verify_inochi2d_runtime.ps1"
$LoadProbePath = Join-Path $RepositoryRoot `
    "tests/scripts/Inochi2dRuntimeLoadProbeTest.ps1"
$ManifestHeader = Join-Path $RepositoryRoot `
    "src/avatar/inochi2d/Inochi2dRuntimeManifest.cpp"

foreach ($Required in @(
    $BootstrapPath, $VerifierPath, $LoadProbePath, $ManifestHeader)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Missing Inochi2D policy artifact: $Required"
    }
}

$Bootstrap = Get-Content -LiteralPath $BootstrapPath -Raw -Encoding utf8
foreach ($Pattern in @(
    'https://github\.com/Inochi2D/inochi2d/archive/66fa76834b28037db0c871c656563422f697879e\.tar\.gz',
    '79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb',
    '66fa76834b28037db0c871c656563422f697879e',
    '0\.8\.7-nightly\+66fa768',
    'ExpectedSourceArchiveSha256',
    'Get-FileHash',
    'if \(\$ArchiveHash -ne \$ExpectedSourceArchiveSha256\)',
    'ValidateSet\("windows-x64"\)',
    'dub',
    '--config=dynamic',
    '--compiler=ldc2',
    '--build=release',
    'IN_VEC2_POSITION',
    'runtime-manifest\.json',
    'verify_inochi2d_runtime\.ps1',
    'BSD-2-Clause'
)) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "Inochi2D bootstrap is missing required policy evidence: $Pattern"
    }
}

foreach ($Pattern in @(
    'imagefmt"\s*=\s*"2\.1\.2"',
    'inmath"\s*=\s*"1\.3\.0"',
    'intel-intrinsics"\s*=\s*"1\.12\.1"',
    'nulib"\s*=\s*"0\.3\.5"',
    'numem"\s*=\s*"1\.3\.2"',
    'silly"\s*=\s*"1\.1\.1"',
    '10f4182efc4fc3846561ca702b3207493f736639498b2dc61a3adcee2bb18736',
    '865fa85d6c07c5f23207cdf9987207d95547e8303009cd0a028b8e7aa9d5aeae',
    '4e056612b6ebe819fef2e45c19d78427b7e67b3c8650445e7379ed2b30f61519',
    'e4b56c28cd3264c72ba18e21889b9dddd1927b83828d92ebe6b49d559b22e597',
    '771688ea0ac4990e8576de4cdcdb381449d78d9edf7a6a7d55adeccfe46d94cc',
    'ffb78e740db5ab36c216c349ec36548a91c66fd1b69b980c1fd3e912ce8ae73b',
    '--skip-registry=all'
)) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "Inochi2D bootstrap omits a cryptographic dependency pin: $Pattern"
    }
}

foreach ($Pattern in @(
    'THIRD_PARTY_NOTICES\.txt',
    'ReadAllBytes',
    'imagefmt"\s*=\s*"BSD-2-Clause"',
    'inmath"\s*=\s*"MIT"',
    'intel-intrinsics"\s*=\s*"BSL-1\.0"',
    'nulib"\s*=\s*"BSL-1\.0"',
    'numem"\s*=\s*"BSL-1\.0"',
    'silly"\s*=\s*"ISC"',
    'druntime-ldc-shared\.dll',
    'phobos2-ldc-shared\.dll',
    'f33033d32bb3f18c031fce39d02b9268389b121eb204f6237009e7cabbcf45ad',
    '25f313915a3b3b369eb65e529489459f7ebd24306edee3ef8d4bd4a9b7b3d4d4',
    '528d3ccc8e94a99615943925ecef85b37334267da5b1c507775b9fbe8e972a7a',
    'LDC druntime and Phobos',
    'runtime_dependencies'
)) {
    if ($Bootstrap -notmatch $Pattern) {
        throw "Inochi2D bootstrap omits required license evidence: $Pattern"
    }
}

$LoadProbe = Get-Content -LiteralPath $LoadProbePath -Raw -Encoding utf8
foreach ($Pattern in @(
    'LoadLibraryExW',
    'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR',
    'LOAD_LIBRARY_SEARCH_SYSTEM32',
    'GetProcAddress',
    'in_puppet_draw'
)) {
    if ($LoadProbe -notmatch $Pattern) {
        throw "Inochi2D load probe omits hardened evidence: $Pattern"
    }
}

foreach ($Symbol in @(
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
)) {
    if ($Bootstrap -notmatch [regex]::Escape($Symbol)) {
        throw "Inochi2D bootstrap omits required runtime symbol: $Symbol"
    }
}

$Verifier = Get-Content -LiteralPath $VerifierPath -Raw -Encoding utf8
foreach ($Pattern in @(
    '0\.8\.7-nightly\+66fa768',
    '79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb',
    'IN_VEC2_POSITION',
    'ExpectedTarget',
    'Get-FileHash',
    'unexpected',
    'runtime-manifest\.json',
    'Get-WindowsX64DllInfo',
    'COFF characteristics',
    'export name table',
    'import descriptor'
)) {
    if ($Verifier -notmatch $Pattern) {
        throw "Inochi2D runtime verifier is missing a fail-closed check: $Pattern"
    }
}
foreach ($UnsupportedTarget in @("macos-arm64", "android-arm64")) {
    if ($Bootstrap -match [regex]::Escape($UnsupportedTarget) -or
        $Verifier -match [regex]::Escape($UnsupportedTarget)) {
        throw "Inochi2D scripts advertise unsupported target: $UnsupportedTarget"
    }
}

$CppVerifier = Get-Content -LiteralPath $ManifestHeader -Raw -Encoding utf8
foreach ($Pattern in @(
    '0\.8\.7-nightly\+66fa768',
    'sha256File',
    'IN_VEC2_POSITION',
    'in_puppet_draw',
    'runtime-manifest\.json'
)) {
    if ($CppVerifier -notmatch $Pattern) {
        throw "C++ Inochi2D verifier is missing a fail-closed check: $Pattern"
    }
}

$CMake = Get-Content -LiteralPath (Join-Path $RepositoryRoot "CMakeLists.txt") `
    -Raw -Encoding utf8
if ($CMake -notmatch 'option\(CS_ENABLE_INOCHI2D\s+"[^"]*"\s+OFF\)') {
    throw "CS_ENABLE_INOCHI2D must be declared and default to OFF"
}
foreach ($Pattern in @(
    'CS_ENABLE_INOCHI2D requires CS_INOCHI2D_ROOT',
    'verify_inochi2d_runtime\.ps1'
)) {
    if ($CMake -notmatch $Pattern) {
        throw "Root CMake is missing the audited Inochi2D gate: $Pattern"
    }
}

$Bom = Get-Content -LiteralPath (Join-Path $RepositoryRoot "legal/OSS_BOM.csv") `
    -Raw -Encoding utf8
$Row = $Bom -split "\r?\n" | Where-Object { $_ -match '^Inochi2D,' }
foreach ($Pattern in @(
    'BSD-2-Clause',
    'Windows x64 only',
    '0\.8\.7-nightly\+66fa768',
    '66fa76834b28037db0c871c656563422f697879e',
    '79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb'
)) {
    if ($Row -notmatch $Pattern) {
        throw "Inochi2D OSS BOM row is missing audited evidence: $Pattern"
    }
}
foreach ($DependencyRow in @(
    "Inochi2D imagefmt",
    "Inochi2D inmath",
    "Inochi2D intel-intrinsics",
    "Inochi2D nulib",
    "Inochi2D numem",
    "Inochi2D silly",
    "Inochi2D LDC runtime"
)) {
    if ($Bom -notmatch "(?m)^$([regex]::Escape($DependencyRow)),") {
        throw "Inochi2D OSS BOM is missing dependency row: $DependencyRow"
    }
}

Write-Host "Inochi2D bootstrap policy is fail-closed."
