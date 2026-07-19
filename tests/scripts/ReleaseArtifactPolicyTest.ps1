param([Parameter(Mandatory)] [string] $RepositoryRoot)
$ErrorActionPreference = 'Stop'

$verifier = Join-Path $RepositoryRoot 'scripts/verify_release_artifacts.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('cs-release-artifact-policy-' + [guid]::NewGuid().ToString('N'))

function Write-Json([string] $Path, [object] $Value) {
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 8) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Invoke-ExpectedFailure([hashtable] $Arguments, [string] $Case) {
    $failed = $false
    try {
        & $verifier @Arguments | Out-Null
    } catch {
        $failed = $true
    }
    if (-not $failed) { throw "$Case unexpectedly passed release validation." }
}

try {
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $artifact = Join-Path $root 'CreatorStudio.msix'
    [IO.File]::WriteAllText($artifact, 'signed release bytes')
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = Join-Path $root 'release-manifest.json'
    Write-Json $manifest ([ordered]@{
        schemaVersion = 1
        productVersion = '1.0.0'
        sourceRevision = 'abc1234'
        target = 'windows-x64'
        artifacts = @([ordered]@{ path = 'CreatorStudio.msix'; sha256 = $hash })
    })
    $evidence = Join-Path $root 'evidence'
    New-Item -ItemType Directory -Path $evidence | Out-Null
    [IO.File]::WriteAllText((Join-Path $evidence 'OSS_BOM.csv'), "component,license,status`nQt 6,LGPL-3.0,APPROVED_WITH_OBLIGATIONS`n")
    [IO.File]::WriteAllText((Join-Path $evidence 'THIRD_PARTY_NOTICES.txt'), "Creator Studio third-party notices`n")

    $arguments = @{
        Platform = 'Windows'
        Artifact = $artifact
        Manifest = $manifest
        EvidenceRoot = $evidence
    }

    Invoke-ExpectedFailure $arguments 'unsigned artifact'

    Write-Json (Join-Path $evidence 'windows-authenticode.json') ([ordered]@{
        schemaVersion = 1
        artifactSha256 = $hash
        status = 'Valid'
        signerCertificateThumbprint = ('a' * 40)
        signerSubject = 'CN=Studio On Air Release'
    })
    $badManifest = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $badManifest.artifacts[0].sha256 = ('0' * 64)
    Write-Json $manifest $badManifest
    Invoke-ExpectedFailure $arguments 'hash-mismatched manifest'

    $badManifest.artifacts[0].sha256 = $hash
    Write-Json $manifest $badManifest
    Remove-Item -LiteralPath (Join-Path $evidence 'THIRD_PARTY_NOTICES.txt')
    Invoke-ExpectedFailure $arguments 'missing notice'
    [IO.File]::WriteAllText((Join-Path $evidence 'THIRD_PARTY_NOTICES.txt'), "Creator Studio third-party notices`n")

    & $verifier @arguments | Out-Null

    $macArtifact = Join-Path $root 'CreatorStudio.pkg'
    [IO.File]::WriteAllText($macArtifact, 'notarized mac release bytes')
    $macHash = (Get-FileHash -LiteralPath $macArtifact -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Json $manifest ([ordered]@{
        schemaVersion = 1; productVersion = '1.0.0'; sourceRevision = 'abc1234'; target = 'macos-arm64'
        artifacts = @([ordered]@{ path = 'CreatorStudio.pkg'; sha256 = $macHash })
    })
    Write-Json (Join-Path $evidence 'macos-signing.json') ([ordered]@{
        schemaVersion = 1; artifactSha256 = $macHash; codesignStatus = 'valid'
        notarizationStatus = 'accepted'; teamIdentifier = 'ABCDE12345'
    })
    & $verifier -Platform macOS -Artifact $macArtifact -Manifest $manifest -EvidenceRoot $evidence | Out-Null

    $androidArtifact = Join-Path $root 'CreatorStudio.aab'
    [IO.File]::WriteAllText($androidArtifact, 'play signing release bytes')
    $androidHash = (Get-FileHash -LiteralPath $androidArtifact -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Json $manifest ([ordered]@{
        schemaVersion = 1; productVersion = '1.0.0'; sourceRevision = 'abc1234'; target = 'android-arm64'
        artifacts = @([ordered]@{ path = 'CreatorStudio.aab'; sha256 = $androidHash })
    })
    Write-Json (Join-Path $evidence 'android-signing.json') ([ordered]@{
        schemaVersion = 1; artifactSha256 = $androidHash; verified = $true
        certificateSha256Digest = ('b' * 64); packageType = 'aab'
    })
    & $verifier -Platform Android -Artifact $androidArtifact -Manifest $manifest -EvidenceRoot $evidence | Out-Null

    Remove-Item -LiteralPath (Join-Path $evidence 'android-signing.json')
    Write-Json (Join-Path $evidence 'distribution-mode.json') ([ordered]@{
        schemaVersion = 1; artifactSha256 = $androidHash; publishable = $false
        reason = 'ci-unsigned-evidence-only'
    })
    & $verifier -Platform Android -Artifact $androidArtifact -Manifest $manifest -EvidenceRoot $evidence -NonPublishable | Out-Null
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
