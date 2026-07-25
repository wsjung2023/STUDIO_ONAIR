[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    throw "The current hardened load probe is implemented only for Windows"
}

$RuntimeRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)
$LibraryPath = Join-Path $RuntimeRoot "bin/inochi2d.dll"
if (-not (Test-Path -LiteralPath $LibraryPath -PathType Leaf)) {
    throw "Inochi2D runtime library is missing: $LibraryPath"
}

$NativeSource = @'
using System;
using System.Runtime.InteropServices;

public static class Inochi2dLoadProbeNative
{
    public const uint LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100;
    public const uint LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x00000800;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(
        string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
if (-not ("Inochi2dLoadProbeNative" -as [type])) {
    Add-Type -TypeDefinition $NativeSource
}

$Flags =
    [Inochi2dLoadProbeNative]::LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR -bor
    [Inochi2dLoadProbeNative]::LOAD_LIBRARY_SEARCH_SYSTEM32
$Module = [Inochi2dLoadProbeNative]::LoadLibraryExW(
    $LibraryPath, [IntPtr]::Zero, $Flags)
if ($Module -eq [IntPtr]::Zero) {
    $Code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "Clean Inochi2D runtime load failed with Win32 error $Code"
}
try {
    foreach ($Symbol in @(
        "in_puppet_load",
        "in_puppet_free",
        "in_puppet_get_parameters",
        "in_parameter_get_name",
        "in_parameter_get_dimensions",
        "in_parameter_set_value",
        "in_puppet_update",
        "in_puppet_draw",
        "in_puppet_get_drawlist",
        "in_drawlist_get_commands",
        "in_drawlist_get_vertex_data",
        "in_drawlist_get_index_data",
        "in_texture_get_width",
        "in_texture_get_height",
        "in_texture_get_channels",
        "in_texture_get_pixels"
    )) {
        if ([Inochi2dLoadProbeNative]::GetProcAddress($Module, $Symbol) -eq
            [IntPtr]::Zero) {
            throw "Loaded Inochi2D runtime is missing export: $Symbol"
        }
    }
}
finally {
    [Inochi2dLoadProbeNative]::FreeLibrary($Module) | Out-Null
}

Write-Host "Clean hardened Inochi2D load and all 16 exports succeeded."
