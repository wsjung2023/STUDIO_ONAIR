param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$backend = Join-Path $RepositoryRoot 'src/app/android/AndroidDeviceCaptureBackend.cpp'
$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$main = Join-Path $RepositoryRoot 'src/main.cpp'

$checks = @(
    @{ Path = $backend; Pattern = 'CameraCaptureFrameAssembler'; Name = 'camera frame adapter' },
    @{ Path = $backend; Pattern = 'AudioCaptureBlockAssembler'; Name = 'microphone PCM adapter' },
    @{ Path = $backend; Pattern = 'createSystemAudio'; Name = 'explicit system-audio capability boundary' },
    @{ Path = $activity; Pattern = 'CameraManager'; Name = 'Camera2 capture boundary' },
    @{ Path = $activity; Pattern = 'AudioRecord'; Name = 'AudioRecord capture boundary' },
    @{ Path = $activity; Pattern = 'requestMediaPermission'; Name = 'runtime permission boundary' },
    @{ Path = $main; Pattern = 'makeAndroidDeviceCaptureBackend'; Name = 'Android application wiring' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path) -or
        -not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android device capture boundary: $($check.Name)"
    }
}
