[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $ArtifactRoot,
    [Parameter(Mandatory)] [string] $OutputPath,
    [Parameter(Mandatory)] [string] $ProductVersion,
    [Parameter(Mandatory)] [string] $SourceRevision,
    [Parameter(Mandatory)] [string] $Target,
    [Parameter(Mandatory)] [string[]] $Artifact
)

$ErrorActionPreference = 'Stop'

function Require-Token([string] $Name, [string] $Value) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value -match '[\s\p{Cc}]') {
        throw "$Name must be a non-empty token."
    }
}

Require-Token 'ProductVersion' $ProductVersion
Require-Token 'SourceRevision' $SourceRevision
Require-Token 'Target' $Target
if ($Artifact.Count -eq 0) { throw 'At least one artifact is required.' }

$root = [IO.Path]::GetFullPath($ArtifactRoot)
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'ArtifactRoot must be a directory.' }
$rootPrefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$entries = foreach ($relative in $Artifact) {
    if ([string]::IsNullOrWhiteSpace($relative) -or $relative.Contains('\') -or [IO.Path]::IsPathRooted($relative)) {
        throw "Artifact path must be a relative forward-slash path: $relative"
    }
    $full = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $full.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Artifact is missing or escapes ArtifactRoot: $relative"
    }
    if (-not $seen.Add($relative)) { throw "Artifact was supplied twice: $relative" }
    [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() }
}

$document = [ordered]@{
    schemaVersion = 1
    productVersion = $ProductVersion
    sourceRevision = $SourceRevision
    target = $Target
    artifacts = @($entries | Sort-Object path)
}

$destination = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
$temporary = Join-Path ([IO.Path]::GetDirectoryName($destination)) ('.' + [IO.Path]::GetFileName($destination) + '.part-' + [guid]::NewGuid().ToString('N'))
try {
    [IO.File]::WriteAllText($temporary, ($document | ConvertTo-Json -Depth 4) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $destination) {
        [IO.File]::Replace($temporary, $destination, $null)
    } else {
        [IO.File]::Move($temporary, $destination)
    }
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
