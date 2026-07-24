[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = "Stop"
$ExpectedVersion = "3.1.2"
$ExpectedSourceTreeSha256 = "1638d4237f6a050f05f7e1eb5928d302916717a4f9c6ccb0d01e75735d512a76"
$ExpectedSourceFileCount = 11
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptRoot "canonical_tree_digest.ps1")

$Tree = Get-CanonicalTreeDigest -Root $SourceRoot
if ($Tree.digest -ne $ExpectedSourceTreeSha256 -or
    @($Tree.files).Count -ne $ExpectedSourceFileCount) {
    throw "miniz source tree does not match the pinned official 3.1.2 archive"
}

foreach ($RequiredPath in @("miniz.c", "miniz.h", "LICENSE")) {
    if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot $RequiredPath) -PathType Leaf)) {
        throw "Pinned miniz source is missing: $RequiredPath"
    }
}

Write-Host "Verified miniz $ExpectedVersion canonical source tree: $($Tree.digest)"
