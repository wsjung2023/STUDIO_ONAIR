# Computes a deterministic digest for an audited file tree.
#
# Serialization, in ordinal lexical order by normalized relative path:
#   UTF-8(relative path with '/' separators) || NUL || raw SHA-256(file bytes)
#
# Entries are concatenated without another delimiter; the fixed 32-byte file
# digest makes each boundary unambiguous after the NUL-terminated path. The
# returned tree digest is SHA-256 over that complete byte stream.
function Get-CanonicalTreeDigest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string[]]$ExcludedRelativePaths = @(),
        [string[]]$AllowedExtensions = @()
    )

    $Root = [System.IO.Path]::GetFullPath($Root)
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Canonical tree root is missing: $Root"
    }

    $Excluded = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::Ordinal
    )
    foreach ($Relative in $ExcludedRelativePaths) {
        if (-not $Excluded.Add($Relative)) {
            throw "Duplicate canonical tree exclusion: $Relative"
        }
    }

    $Extensions = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($Extension in $AllowedExtensions) {
        if (-not $Extensions.Add($Extension)) {
            throw "Duplicate allowed canonical tree extension: $Extension"
        }
    }

    $RootPrefix = $Root.TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    $RootUri = New-Object System.Uri($RootPrefix)
    $Paths = New-Object 'System.Collections.Generic.List[string]'
    $FilesByPath = @{}

    foreach ($Directory in Get-ChildItem -LiteralPath $Root -Recurse -Directory -Force) {
        if (($Directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse directory is forbidden in canonical tree: $($Directory.FullName)"
        }
    }

    foreach ($File in Get-ChildItem -LiteralPath $Root -Recurse -File -Force) {
        if (($File.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse file is forbidden in canonical tree: $($File.FullName)"
        }
        $FileUri = New-Object System.Uri([System.IO.Path]::GetFullPath($File.FullName))
        $Relative = [System.Uri]::UnescapeDataString(
            $RootUri.MakeRelativeUri($FileUri).ToString()
        ).Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($Relative) -or
            $Relative.Contains('\') -or
            [System.IO.Path]::IsPathRooted($Relative) -or
            $Relative -match '(^|/)\.\.(/|$)') {
            throw "Invalid canonical tree path: $Relative"
        }
        if ($Excluded.Contains($Relative)) {
            continue
        }
        if ($Extensions.Count -gt 0 -and
            -not $Extensions.Contains([System.IO.Path]::GetExtension($Relative))) {
            throw "Unapproved file type in canonical tree: $Relative"
        }
        if ($FilesByPath.ContainsKey($Relative)) {
            throw "Duplicate canonical tree path: $Relative"
        }
        $Paths.Add($Relative)
        $FilesByPath[$Relative] = $File.FullName
    }
    $Paths.Sort([System.StringComparer]::Ordinal)

    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $Serialization = New-Object System.IO.MemoryStream
    $FileHasher = [System.Security.Cryptography.SHA256]::Create()
    $TreeHasher = [System.Security.Cryptography.SHA256]::Create()
    $Entries = @()
    try {
        foreach ($Relative in $Paths) {
            $PathBytes = $Utf8NoBom.GetBytes($Relative)
            $Serialization.Write($PathBytes, 0, $PathBytes.Length)
            $Serialization.WriteByte(0)

            $FileStream = [System.IO.File]::OpenRead($FilesByPath[$Relative])
            try {
                $RawHash = $FileHasher.ComputeHash($FileStream)
            } finally {
                $FileStream.Dispose()
            }
            $Serialization.Write($RawHash, 0, $RawHash.Length)
            $Entries += [ordered]@{
                path = $Relative
                sha256 = ([System.BitConverter]::ToString($RawHash)).Replace('-', '').ToLowerInvariant()
            }
        }
        $TreeHash = $TreeHasher.ComputeHash($Serialization.ToArray())
    } finally {
        $FileHasher.Dispose()
        $TreeHasher.Dispose()
        $Serialization.Dispose()
    }

    return [pscustomobject]@{
        digest = ([System.BitConverter]::ToString($TreeHash)).Replace('-', '').ToLowerInvariant()
        files = $Entries
    }
}
