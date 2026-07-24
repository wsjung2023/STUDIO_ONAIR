[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$RuntimeRoot = "",
    [string]$ScratchRoot = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    $RuntimeRoot = Join-Path $RepositoryRoot "build/sodium/prefix"
}
if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $ScratchRoot = Join-Path $RepositoryRoot "build/sodium-runtime-trust-test"
}
$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
$ScratchRoot = [System.IO.Path]::GetFullPath($ScratchRoot)
$BuildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $RepositoryRoot "build")
).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $ScratchRoot.StartsWith(
    $BuildRoot,
    [System.StringComparison]::OrdinalIgnoreCase
)) {
    throw "ScratchRoot must remain inside the repository build directory"
}

$VerifierPath = Join-Path $RepositoryRoot "scripts/verify_sodium_runtime.ps1"
foreach ($RequiredPath in @(
    $VerifierPath,
    (Join-Path $RuntimeRoot "runtime-manifest.json"),
    (Join-Path $RuntimeRoot "bin/libsodium.dll"),
    (Join-Path $RuntimeRoot "lib/libsodium.lib"),
    (Join-Path $RuntimeRoot "include/sodium/crypto_sign.h")
)) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "Runtime trust test input is missing: $RequiredPath"
    }
}

if (Test-Path -LiteralPath $ScratchRoot) {
    Remove-Item -LiteralPath $ScratchRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function New-CasePrefix {
    param([string]$Name)
    $CaseRoot = Join-Path $ScratchRoot $Name
    Copy-Item -LiteralPath $RuntimeRoot -Destination $CaseRoot -Recurse
    return $CaseRoot
}

function Read-Manifest {
    param([string]$CaseRoot)
    return Get-Content -LiteralPath (Join-Path $CaseRoot "runtime-manifest.json") `
        -Raw -Encoding utf8 | ConvertFrom-Json
}

function Write-Manifest {
    param(
        [string]$CaseRoot,
        [object]$Manifest
    )
    [System.IO.File]::WriteAllText(
        (Join-Path $CaseRoot "runtime-manifest.json"),
        ($Manifest | ConvertTo-Json -Depth 8),
        $Utf8NoBom
    )
}

function Set-ManifestFileHash {
    param(
        [object]$Manifest,
        [string]$RelativePath,
        [string]$Sha256,
        [bool]$IsDll
    )
    foreach ($Entry in @($Manifest.files)) {
        if ($Entry.path -eq $RelativePath) {
            $Entry.sha256 = $Sha256
        }
    }
    if ($IsDll) {
        foreach ($Entry in @($Manifest.dlls)) {
            if ($Entry.path -eq $RelativePath) {
                $Entry.sha256 = $Sha256
            }
        }
    }
}

function Invoke-Verifier {
    param([string]$CaseRoot)
    $SavedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $Output = @(
        & powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $VerifierPath `
            -RuntimeRoot $CaseRoot 2>&1
    ) | Out-String
    $ExitCode = $LASTEXITCODE
    $ErrorActionPreference = $SavedPreference
    return [pscustomobject]@{
        exit_code = $ExitCode
        output = $Output
    }
}

$Baseline = Invoke-Verifier -CaseRoot $RuntimeRoot
if ($Baseline.exit_code -ne 0) {
    throw "Baseline audited runtime failed verification: $($Baseline.output)"
}

$AcceptedCases = @()
$WrongFailureCases = @()

$DllCase = New-CasePrefix -Name "dll-and-manifest"
$DllPath = Join-Path $DllCase "bin/libsodium.dll"
[System.IO.File]::AppendAllText($DllPath, "tampered-dll")
$DllHash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash.ToLowerInvariant()
$DllManifest = Read-Manifest -CaseRoot $DllCase
Set-ManifestFileHash `
    -Manifest $DllManifest `
    -RelativePath "bin/libsodium.dll" `
    -Sha256 $DllHash `
    -IsDll $true
Write-Manifest -CaseRoot $DllCase -Manifest $DllManifest
$DllResult = Invoke-Verifier -CaseRoot $DllCase
if ($DllResult.exit_code -eq 0) {
    $AcceptedCases += "DLL plus manifest hash change"
} elseif ($DllResult.output -notmatch "pinned trust anchor") {
    $WrongFailureCases += "DLL plus manifest hash change"
}

$HeaderCase = New-CasePrefix -Name "header-removal-and-manifest"
$RemovedHeader = "include/sodium/crypto_sign.h"
Remove-Item -LiteralPath (Join-Path $HeaderCase $RemovedHeader) -Force
$HeaderManifest = Read-Manifest -CaseRoot $HeaderCase
$HeaderManifest.files = @(
    $HeaderManifest.files | Where-Object { $_.path -ne $RemovedHeader }
)
Write-Manifest -CaseRoot $HeaderCase -Manifest $HeaderManifest
$HeaderResult = Invoke-Verifier -CaseRoot $HeaderCase
if ($HeaderResult.exit_code -eq 0) {
    $AcceptedCases += "header removal plus manifest list change"
} elseif ($HeaderResult.output -notmatch "pinned trust anchor") {
    $WrongFailureCases += "header removal plus manifest list change"
}

$LibraryCase = New-CasePrefix -Name "library-and-manifest"
$LibraryPath = Join-Path $LibraryCase "lib/libsodium.lib"
[System.IO.File]::AppendAllText($LibraryPath, "tampered-library")
$LibraryHash = (Get-FileHash -LiteralPath $LibraryPath -Algorithm SHA256).Hash.ToLowerInvariant()
$LibraryManifest = Read-Manifest -CaseRoot $LibraryCase
Set-ManifestFileHash `
    -Manifest $LibraryManifest `
    -RelativePath "lib/libsodium.lib" `
    -Sha256 $LibraryHash `
    -IsDll $false
Write-Manifest -CaseRoot $LibraryCase -Manifest $LibraryManifest
$LibraryResult = Invoke-Verifier -CaseRoot $LibraryCase
if ($LibraryResult.exit_code -eq 0) {
    $AcceptedCases += "import library plus manifest hash change"
} elseif ($LibraryResult.output -notmatch "pinned trust anchor") {
    $WrongFailureCases += "import library plus manifest hash change"
}

if ($AcceptedCases.Count -gt 0) {
    throw "Verifier accepted tampered canonical tree cases: $($AcceptedCases -join '; ')"
}
if ($WrongFailureCases.Count -gt 0) {
    throw "Tampered cases failed outside the pinned canonical trust anchor: $($WrongFailureCases -join '; ')"
}

Write-Host "Pinned libsodium canonical tree rejects manifest-assisted tampering."
