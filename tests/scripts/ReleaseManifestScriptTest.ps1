param([Parameter(Mandatory)] [string] $RepositoryRoot)
$ErrorActionPreference = 'Stop'
$dir = Join-Path ([IO.Path]::GetTempPath()) ('cs-release-script-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path (Join-Path $dir 'bin') -Force | Out-Null
    $artifact = Join-Path $dir 'bin/CreatorStudio.exe'
    [IO.File]::WriteAllText($artifact, 'release bytes')
    $output = Join-Path $dir 'release-manifest.json'
    & (Join-Path $RepositoryRoot 'scripts/write_release_manifest.ps1') -ArtifactRoot $dir -OutputPath $output -ProductVersion '1.0.0' -SourceRevision 'abc1234' -Target 'windows-x64' -Artifact 'bin/CreatorStudio.exe'
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) { throw 'Release manifest script did not write its output.' }
    $manifest = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    if ($manifest.artifacts.Count -ne 1) { throw 'Release manifest artifact count is wrong.' }
    $expected = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest.artifacts[0].sha256 -ne $expected) { throw 'Release manifest SHA-256 is wrong.' }
    [IO.File]::WriteAllText($artifact, 'tampered')
    $actual = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest.artifacts[0].sha256 -eq $actual) { throw 'Tampered artifact unexpectedly retained its hash.' }
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
