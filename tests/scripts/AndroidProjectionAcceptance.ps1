[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [string]$DeviceSerial = ''
)

$ErrorActionPreference = 'Stop'

$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$backend = Join-Path $RepositoryRoot 'src/app/android/AndroidScreenCaptureBackend.cpp'
$session = Join-Path $RepositoryRoot 'src/capture/AndroidProjectionSession.cpp'

$checks = @(
    @{ Path = $activity; Pattern = 'MediaProjection.Callback'; Name = 'Android projection revocation callback' },
    @{ Path = $activity; Pattern = 'HandlerThread\("CreatorStudioProjection"\)'; Name = 'off-UI projection frame thread' },
    @{ Path = $activity; Pattern = 'acquireLatestImage'; Name = 'bounded latest-image acquisition' },
    @{ Path = $activity; Pattern = 'nativeProjectionRevoked'; Name = 'native revocation callback' },
    @{ Path = $activity; Pattern = 'nativeProjectionReleased'; Name = 'native release-complete callback' },
    @{ Path = $activity; Pattern = 'releaseProjection\(approvedGeneration, true, true\)'; Name = 'activity destruction release' },
    @{ Path = $backend; Pattern = 'requestStop'; Name = 'native asynchronous stop barrier' },
    @{ Path = $backend; Pattern = 'queueOnApplicationThread'; Name = 'Qt-thread lifecycle delivery' },
    @{ Path = $backend; Pattern = 'std::shared_ptr<ProjectionSourceState>'; Name = 'callback-safe source lifetime' },
    @{ Path = $session; Pattern = 'ProjectionSessionState::Stopping'; Name = 'explicit native stopping state' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path)) {
        throw "missing Android projection acceptance file: $($check.Path)"
    }
    if (-not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android projection acceptance boundary: $($check.Name)"
    }
}

if ([string]::IsNullOrWhiteSpace($DeviceSerial)) {
    return
}

$sdkAdb = if ([string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
    $null
} else {
    Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe'
}
$adbCandidates = @(
    (Get-Command adb -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
    $sdkAdb,
    'C:\AndroidSDK\platform-tools\adb.exe'
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$adb = $adbCandidates | Select-Object -First 1
if (-not $adb) {
    throw 'adb was not found; set ANDROID_SDK_ROOT or add platform-tools to PATH'
}

$deviceLine = & $adb devices -l | Select-String -Pattern "^$([regex]::Escape($DeviceSerial))\s+device\b"
if (-not $deviceLine) {
    throw "Android device '$DeviceSerial' is not connected and authorized"
}

& $adb -s $DeviceSerial shell getprop ro.build.version.release
if ($LASTEXITCODE -ne 0) {
    throw "could not query Android device '$DeviceSerial'"
}
