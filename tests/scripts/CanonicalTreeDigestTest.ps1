[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$ScratchRoot = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $ScratchRoot = Join-Path $RepositoryRoot "build/canonical-tree-digest-test"
}
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

$HelperPath = Join-Path $RepositoryRoot "scripts/canonical_tree_digest.ps1"
. $HelperPath
if (Test-Path -LiteralPath $ScratchRoot) {
    Remove-Item -LiteralPath $ScratchRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null

function New-SingleFileTree {
    param(
        [string]$Name,
        [string]$FileName
    )
    $Root = Join-Path $ScratchRoot $Name
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    [System.IO.File]::WriteAllBytes(
        (Join-Path $Root $FileName),
        [System.Text.Encoding]::UTF8.GetBytes("same-content")
    )
    return $Root
}

$Problems = @()
$ATree = Get-CanonicalTreeDigest -Root (New-SingleFileTree -Name "plain" -FileName "A")
$PercentTree = Get-CanonicalTreeDigest `
    -Root (New-SingleFileTree -Name "percent" -FileName "%41")
if (@($ATree.files).Count -ne 1 -or $ATree.files[0].path -cne "A") {
    $Problems += "Physical file A did not retain the literal entry path A"
}
if (@($PercentTree.files).Count -ne 1 -or $PercentTree.files[0].path -cne "%41") {
    $Problems += "Physical file %41 was URI-decoded instead of remaining literal"
}
if ($ATree.digest -eq $PercentTree.digest) {
    $Problems += "Physical names A and %41 produced the same canonical digest"
}

$InvalidRelativePaths = @(
    "",
    "C:/absolute",
    ".",
    "..",
    "segment/../file",
    "segment\file",
    ("control{0}file" -f [char]1),
    ("nul{0}file" -f [char]0)
)
foreach ($InvalidRelativePath in $InvalidRelativePaths) {
    $Rejected = $false
    try {
        Get-CanonicalTreeDigest `
            -Root (Join-Path $ScratchRoot "plain") `
            -ExcludedRelativePaths @($InvalidRelativePath) | Out-Null
    } catch {
        if ($_ -match "Invalid canonical tree exclusion") {
            $Rejected = $true
        }
    }
    if (-not $Rejected) {
        $Problems += "Invalid relative path was accepted by canonical tree helper"
    }
}

$HelperSource = Get-Content -LiteralPath $HelperPath -Raw -Encoding utf8
if ($HelperSource -match 'System\.Uri|UnescapeDataString') {
    $Problems += "Canonical tree helper still uses URI decoding"
}
if ($HelperSource -notmatch '\\x00-\\x1F\\x7F') {
    $Problems += "Canonical tree helper does not explicitly reject control characters"
}

$JunctionTarget = Join-Path $ScratchRoot "junction-target"
$JunctionRoot = Join-Path $ScratchRoot "junction-root"
New-Item -ItemType Directory -Force -Path $JunctionTarget | Out-Null
[System.IO.File]::WriteAllText((Join-Path $JunctionTarget "file.h"), "content")
$JunctionCreated = $false
try {
    New-Item -ItemType Junction -Path $JunctionRoot -Target $JunctionTarget `
        -ErrorAction Stop | Out-Null
    $JunctionCreated = $true
} catch {
    Write-Host "Junction creation unavailable; using source-level root reparse assertion."
}
if ($JunctionCreated) {
    $Rejected = $false
    try {
        Get-CanonicalTreeDigest -Root $JunctionRoot | Out-Null
    } catch {
        if ($_ -match "root.*reparse|reparse.*root") {
            $Rejected = $true
        }
    }
    if (-not $Rejected) {
        $Problems += "Canonical tree helper accepted a reparse-point root"
    }
} elseif ($HelperSource -notmatch
    'Get-Item[^\r\n]*\$Root[\s\S]*FileAttributes\]::ReparsePoint') {
    $Problems += "Canonical tree helper has no explicit root reparse-point guard"
}

if ($Problems.Count -gt 0) {
    throw "Canonical tree regressions failed: $($Problems -join '; ')"
}

Write-Host "Canonical tree paths preserve literal names and reject reparse roots."
