[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Windows', 'macOS', 'Android')]
    [string] $Platform,
    [Parameter(Mandatory)] [string] $Artifact,
    [Parameter(Mandatory)] [string] $Manifest,
    [Parameter(Mandatory)] [string] $EvidenceRoot,
    [switch] $NonPublishable
)

$ErrorActionPreference = 'Stop'

function Require-RegularFile([string] $Path, [string] $Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $attributes = [IO.File]::GetAttributes([IO.Path]::GetFullPath($Path))
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a link or reparse point: $Path"
    }
}

function Read-StrictJson([string] $Path, [string[]] $Properties, [string] $Description) {
    Require-RegularFile $Path $Description
    $value = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $actual = @($value.PSObject.Properties.Name | Sort-Object)
    $expected = @($Properties | Sort-Object)
    if ([string]::Join('|', $actual) -cne [string]::Join('|', $expected)) {
        throw "$Description has unknown or missing fields."
    }
    return $value
}

function Require-Sha256([string] $Value, [string] $Description) {
    if ($Value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Description must be a canonical lowercase SHA-256 value."
    }
}

function Require-ArtifactType([string] $PlatformName, [string] $Path, [bool] $EvidenceOnly) {
    $extension = [IO.Path]::GetExtension($Path).ToLowerInvariant()
    switch ($PlatformName) {
        'Windows' {
            if ($extension -notin @('.exe', '.msix', '.msi')) {
                throw 'Windows artifact must be an EXE, MSIX, or MSI file.'
            }
        }
        'macOS' {
            if (-not $EvidenceOnly -and $extension -notin @('.dmg', '.pkg', '.zip')) {
                throw 'Publishable macOS artifact must be a DMG, PKG, or ZIP file.'
            }
        }
        'Android' {
            if ($extension -notin @('.apk', '.aab')) {
                throw 'Android artifact must be an APK or AAB file.'
            }
        }
    }
}

$artifactPath = [IO.Path]::GetFullPath($Artifact)
$manifestPath = [IO.Path]::GetFullPath($Manifest)
$evidencePath = [IO.Path]::GetFullPath($EvidenceRoot)
Require-RegularFile $artifactPath 'release artifact'
Require-RegularFile $manifestPath 'release manifest'
if (-not (Test-Path -LiteralPath $evidencePath -PathType Container)) {
    throw "EvidenceRoot is missing: $evidencePath"
}
Require-ArtifactType $Platform $artifactPath $NonPublishable.IsPresent

$manifestDocument = Read-StrictJson $manifestPath `
    @('schemaVersion', 'productVersion', 'sourceRevision', 'target', 'artifacts') `
    'release manifest'
if ($manifestDocument.schemaVersion -ne 1 -or
    [string]::IsNullOrWhiteSpace([string]$manifestDocument.productVersion) -or
    [string]::IsNullOrWhiteSpace([string]$manifestDocument.sourceRevision)) {
    throw 'Release manifest identity fields are invalid.'
}
$targetPattern = switch ($Platform) {
    'Windows' { '^windows(?:-|$)' }
    'macOS' { '^macos(?:-|$)' }
    'Android' { '^android(?:-|$)' }
}
if ([string]$manifestDocument.target -notmatch $targetPattern) {
    throw "Release manifest target does not match $Platform."
}

$manifestDirectory = [IO.Path]::GetDirectoryName($manifestPath)
$manifestPrefix = $manifestDirectory.TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$matchingEntries = @()
foreach ($entry in @($manifestDocument.artifacts)) {
    $properties = @($entry.PSObject.Properties.Name | Sort-Object)
    if ([string]::Join('|', $properties) -cne 'path|sha256') {
        throw 'Release manifest artifact has unknown or missing fields.'
    }
    $relative = [string]$entry.path
    if ([string]::IsNullOrWhiteSpace($relative) -or $relative.Contains('\') -or
        [IO.Path]::IsPathRooted($relative)) {
        throw 'Release manifest artifact path is not canonical.'
    }
    $resolved = [IO.Path]::GetFullPath((Join-Path $manifestDirectory $relative))
    if (-not $resolved.StartsWith($manifestPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Release manifest artifact path escapes the manifest directory.'
    }
    Require-Sha256 ([string]$entry.sha256) 'release manifest artifact hash'
    if ($resolved -ieq $artifactPath) { $matchingEntries += $entry }
}
if ($matchingEntries.Count -ne 1) {
    throw 'Release manifest must contain the requested artifact exactly once.'
}
$artifactHash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ([string]$matchingEntries[0].sha256 -cne $artifactHash) {
    throw 'Release artifact SHA-256 does not match the release manifest.'
}

$bom = Join-Path $evidencePath 'OSS_BOM.csv'
$notices = Join-Path $evidencePath 'THIRD_PARTY_NOTICES.txt'
Require-RegularFile $bom 'open-source bill of materials'
Require-RegularFile $notices 'third-party notices'
$bomLines = @(Get-Content -LiteralPath $bom | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($bomLines.Count -lt 2 -or $bomLines[0] -notmatch '^component,') {
    throw 'Open-source bill of materials is empty or malformed.'
}
if ((Get-Item -LiteralPath $notices).Length -lt 20) {
    throw 'Third-party notices are empty.'
}

if ($NonPublishable) {
    $mode = Read-StrictJson (Join-Path $evidencePath 'distribution-mode.json') `
        @('schemaVersion', 'artifactSha256', 'publishable', 'reason') `
        'non-publishable distribution evidence'
    Require-Sha256 ([string]$mode.artifactSha256) 'distribution evidence artifact hash'
    if ($mode.schemaVersion -ne 1 -or $mode.publishable -ne $false -or
        [string]$mode.artifactSha256 -cne $artifactHash -or
        [string]$mode.reason -cne 'ci-unsigned-evidence-only') {
        throw 'Non-publishable distribution evidence is invalid.'
    }
} else {
    switch ($Platform) {
        'Windows' {
            $signing = Read-StrictJson (Join-Path $evidencePath 'windows-authenticode.json') `
                @('schemaVersion', 'artifactSha256', 'status',
                  'signerCertificateThumbprint', 'signerSubject') `
                'Windows Authenticode evidence'
            Require-Sha256 ([string]$signing.artifactSha256) 'Authenticode artifact hash'
            if ($signing.schemaVersion -ne 1 -or $signing.status -cne 'Valid' -or
                [string]$signing.artifactSha256 -cne $artifactHash -or
                [string]$signing.signerCertificateThumbprint -cnotmatch '^[0-9a-fA-F]{40}([0-9a-fA-F]{24})?$' -or
                [string]::IsNullOrWhiteSpace([string]$signing.signerSubject)) {
                throw 'Windows Authenticode evidence is invalid.'
            }
        }
        'macOS' {
            $signing = Read-StrictJson (Join-Path $evidencePath 'macos-signing.json') `
                @('schemaVersion', 'artifactSha256', 'codesignStatus',
                  'notarizationStatus', 'teamIdentifier') `
                'macOS signing and notarization evidence'
            Require-Sha256 ([string]$signing.artifactSha256) 'macOS signing artifact hash'
            if ($signing.schemaVersion -ne 1 -or $signing.codesignStatus -cne 'valid' -or
                $signing.notarizationStatus -cne 'accepted' -or
                [string]$signing.artifactSha256 -cne $artifactHash -or
                [string]$signing.teamIdentifier -cnotmatch '^[A-Z0-9]{10}$') {
                throw 'macOS signing or notarization evidence is invalid.'
            }
        }
        'Android' {
            $signing = Read-StrictJson (Join-Path $evidencePath 'android-signing.json') `
                @('schemaVersion', 'artifactSha256', 'verified',
                  'certificateSha256Digest', 'packageType') `
                'Android signing certificate evidence'
            Require-Sha256 ([string]$signing.artifactSha256) 'Android signing artifact hash'
            Require-Sha256 ([string]$signing.certificateSha256Digest) 'Android certificate digest'
            $expectedPackageType = [IO.Path]::GetExtension($artifactPath).TrimStart('.').ToLowerInvariant()
            if ($signing.schemaVersion -ne 1 -or $signing.verified -ne $true -or
                [string]$signing.artifactSha256 -cne $artifactHash -or
                [string]$signing.packageType -cne $expectedPackageType) {
                throw 'Android signing certificate evidence is invalid.'
            }
        }
    }
}

[pscustomobject]@{
    platform = $Platform
    artifact = $artifactPath
    artifactSha256 = $artifactHash
    publishable = -not $NonPublishable.IsPresent
}
