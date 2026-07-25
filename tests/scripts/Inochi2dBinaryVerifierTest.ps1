[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,
    [Parameter(Mandatory = $true)]
    [string]$VerifierPath,
    [Parameter(Mandatory = $true)]
    [string]$ValidFixture,
    [Parameter(Mandatory = $true)]
    [string]$MissingExportFixture
)

$ErrorActionPreference = "Stop"
$CaseRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "creator-studio-inochi2d-binary-" + [Guid]::NewGuid().ToString("N"))

function Set-LibraryAndHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CasePath,
        [Parameter(Mandatory = $true)]
        [string]$SourceLibrary
    )
    $Destination = Join-Path $CasePath "bin/inochi2d.dll"
    Copy-Item -LiteralPath $SourceLibrary -Destination $Destination -Force
    $ManifestPath = Join-Path $CasePath "runtime-manifest.json"
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $Manifest.library.sha256 =
        (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).
            Hash.ToLowerInvariant()
    $Manifest | ConvertTo-Json -Depth 20 |
        Set-Content -LiteralPath $ManifestPath -Encoding utf8
}

function Assert-VerifierRejects {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CasePath,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )
    $Process = Start-Process -FilePath "powershell.exe" -WindowStyle Hidden `
        -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            "`"$VerifierPath`"", "-RuntimeRoot", "`"$CasePath`"",
            "-ExpectedTarget", "windows-x64") `
        -Wait -PassThru
    if ($Process.ExitCode -eq 0) {
        throw "Verifier accepted $Description"
    }
}

try {
    New-Item -ItemType Directory -Path $CaseRoot | Out-Null

    $MissingCase = Join-Path $CaseRoot "missing-export"
    Copy-Item -LiteralPath $RuntimeRoot -Destination $MissingCase -Recurse
    Set-LibraryAndHash -CasePath $MissingCase `
        -SourceLibrary $MissingExportFixture
    Assert-VerifierRejects -CasePath $MissingCase `
        -Description "a DLL missing an actual required export"

    $WrongTypeCase = Join-Path $CaseRoot "wrong-type"
    Copy-Item -LiteralPath $RuntimeRoot -Destination $WrongTypeCase -Recurse
    Set-LibraryAndHash -CasePath $WrongTypeCase -SourceLibrary $ValidFixture
    $LibraryPath = Join-Path $WrongTypeCase "bin/inochi2d.dll"
    [byte[]]$Bytes = [System.IO.File]::ReadAllBytes($LibraryPath)
    $PeOffset = [BitConverter]::ToUInt32($Bytes, 0x3c)
    $CharacteristicsOffset = [int]$PeOffset + 22
    $Characteristics =
        [BitConverter]::ToUInt16($Bytes, $CharacteristicsOffset)
    $Characteristics = $Characteristics -band (-bnot 0x2000)
    [BitConverter]::GetBytes([uint16]$Characteristics).CopyTo(
        $Bytes, $CharacteristicsOffset)
    [System.IO.File]::WriteAllBytes($LibraryPath, $Bytes)
    $ManifestPath = Join-Path $WrongTypeCase "runtime-manifest.json"
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $Manifest.library.sha256 =
        (Get-FileHash -LiteralPath $LibraryPath -Algorithm SHA256).
            Hash.ToLowerInvariant()
    $Manifest | ConvertTo-Json -Depth 20 |
        Set-Content -LiteralPath $ManifestPath -Encoding utf8
    Assert-VerifierRejects -CasePath $WrongTypeCase `
        -Description "a PE image without IMAGE_FILE_DLL"
}
finally {
    if (Test-Path -LiteralPath $CaseRoot) {
        Remove-Item -LiteralPath $CaseRoot -Recurse -Force
    }
}
