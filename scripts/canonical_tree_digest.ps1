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
    $RootItem = Get-Item -LiteralPath $Root -Force
    if (($RootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Canonical tree root must not be a reparse point: $Root"
    }
    $Root = [System.IO.Path]::GetFullPath($RootItem.FullName).TrimEnd('\', '/')
    $RootPrefix = $Root + [System.IO.Path]::DirectorySeparatorChar
    $PathComparison = if (
        [System.Environment]::OSVersion.Platform -eq
            [System.PlatformID]::Win32NT
    ) {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }

    $Excluded = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::Ordinal
    )
    foreach ($Relative in $ExcludedRelativePaths) {
        if ([string]::IsNullOrEmpty($Relative) -or
            $Relative -match '[\x00-\x1F\x7F]' -or
            $Relative.Contains('\') -or
            [System.IO.Path]::IsPathRooted($Relative) -or
            $Relative.StartsWith('/') -or
            $Relative.EndsWith('/') -or
            $Relative.Contains('//') -or
            $Relative -match '(^|/)(\.|\.\.)(/|$)') {
            throw "Invalid canonical tree exclusion: $Relative"
        }
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
        $FullPath = [System.IO.Path]::GetFullPath($File.FullName)
        if (-not $FullPath.StartsWith($RootPrefix, $PathComparison)) {
            throw "Canonical tree file escaped its root: $FullPath"
        }
        $Relative = $FullPath.Substring($RootPrefix.Length)
        $Relative = $Relative.Replace(
            [System.IO.Path]::DirectorySeparatorChar,
            '/'
        )
        if ([System.IO.Path]::AltDirectorySeparatorChar -ne
            [System.IO.Path]::DirectorySeparatorChar) {
            $Relative = $Relative.Replace(
                [System.IO.Path]::AltDirectorySeparatorChar,
                '/'
            )
        }
        if ([string]::IsNullOrEmpty($Relative) -or
            $Relative -match '[\x00-\x1F\x7F]' -or
            $Relative.Contains('\') -or
            [System.IO.Path]::IsPathRooted($Relative) -or
            $Relative.StartsWith('/') -or
            $Relative.EndsWith('/') -or
            $Relative.Contains('//') -or
            $Relative -match '(^|/)(\.|\.\.)(/|$)') {
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
        $FilesByPath[$Relative] = $FullPath
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
