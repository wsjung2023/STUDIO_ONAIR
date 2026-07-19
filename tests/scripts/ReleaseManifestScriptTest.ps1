param([Parameter(Mandatory)] [string] $RepositoryRoot)
$ErrorActionPreference = 'Stop'
$rootCmake = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'CMakeLists.txt') -Raw
if ($rootCmake -notmatch 'add_custom_target\(release-metadata') {
    throw 'CMake release-metadata target is missing.'
}
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

    & (Join-Path $RepositoryRoot 'scripts/write_release_manifest.ps1') -ArtifactRoot $dir -OutputPath $output -ProductVersion '1.0.1' -SourceRevision 'def5678' -Target 'windows-x64' -Artifact 'bin/CreatorStudio.exe'
    $replaced = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    if ($replaced.productVersion -ne '1.0.1' -or $replaced.sourceRevision -ne 'def5678') {
        throw 'Release manifest did not atomically replace an existing output.'
    }

    $gitOutput = Join-Path $dir 'release-manifest-from-git.json'
    & (Join-Path $RepositoryRoot 'scripts/write_release_manifest.ps1') -ArtifactRoot $dir -OutputPath $gitOutput -ProductVersion '1.0.0' -RepositoryRoot $RepositoryRoot -Target 'windows-x64' -Artifact 'bin/CreatorStudio.exe'
    $gitManifest = Get-Content -LiteralPath $gitOutput -Raw | ConvertFrom-Json
    $head = (& git -C $RepositoryRoot rev-parse --verify HEAD).Trim()
    $dirty = @(& git -C $RepositoryRoot status --porcelain --untracked-files=no)
    $expectedRevision = $head + $(if ($dirty.Count -gt 0) { '-dirty' } else { '' })
    if ($gitManifest.sourceRevision -ne $expectedRevision) {
        throw "Git-derived source revision is not honest: expected $expectedRevision, got $($gitManifest.sourceRevision)."
    }

    [IO.File]::WriteAllText($artifact, 'tampered')
    $actual = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest.artifacts[0].sha256 -eq $actual) { throw 'Tampered artifact unexpectedly retained its hash.' }
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
