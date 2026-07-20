[CmdletBinding(DefaultParameterSetName = 'Explicit')]
param(
    [Parameter(Mandatory)] [string] $ArtifactRoot,
    [Parameter(Mandatory)] [string] $OutputPath,
    [Parameter(Mandatory)] [string] $ProductVersion,
    [Parameter(Mandatory, ParameterSetName = 'Explicit')] [string] $SourceRevision,
    [Parameter(Mandatory, ParameterSetName = 'Git')] [string] $RepositoryRoot,
    [Parameter(Mandatory)] [string] $Target,
    [Parameter(Mandatory)] [string[]] $Artifact
)

$ErrorActionPreference = 'Stop'

function Require-Token([string] $Name, [string] $Value) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value -match '[\s\p{Cc}]') {
        throw "$Name must be a non-empty token."
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Git') {
    $repository = [IO.Path]::GetFullPath($RepositoryRoot)
    $head = & git -C $repository rev-parse --verify HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($head)) {
        throw 'RepositoryRoot must identify a Git worktree with a valid HEAD.'
    }
    $changes = @(& git -C $repository status --porcelain --untracked-files=no 2>$null)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect the Git worktree state.'
    }
    $SourceRevision = $head.Trim()
    if ($changes.Count -gt 0) {
        $SourceRevision += '-dirty'
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
$backup = Join-Path ([IO.Path]::GetDirectoryName($destination)) ('.' + [IO.Path]::GetFileName($destination) + '.backup-' + [guid]::NewGuid().ToString('N'))
try {
    [IO.File]::WriteAllText($temporary, ($document | ConvertTo-Json -Depth 4) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $destination) {
        [IO.File]::Replace($temporary, $destination, $backup)
        Remove-Item -LiteralPath $backup -Force
    } else {
        [IO.File]::Move($temporary, $destination)
    }
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }
}
