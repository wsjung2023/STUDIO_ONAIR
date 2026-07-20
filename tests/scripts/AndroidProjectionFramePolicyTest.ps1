param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'

$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$service = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioProjectionService.java'
$backend = Join-Path $RepositoryRoot 'src/app/android/AndroidScreenCaptureBackend.cpp'
$manifest = Join-Path $RepositoryRoot 'android/AndroidManifest.xml'

$checks = @(
    @{ Path = $activity; Pattern = 'ImageReader'; Name = 'ImageReader capture source' },
    @{ Path = $activity; Pattern = 'createVirtualDisplay'; Name = 'VirtualDisplay capture output' },
    @{ Path = $activity; Pattern = 'nativeProjectionFrame'; Name = 'native frame callback' },
    @{ Path = $activity; Pattern = 'nativeProjectionRevoked'; Name = 'projection revoke callback' },
    @{ Path = $activity; Pattern = 'publish the consent token before notifying it'; Name = 'consent callback ordering' },
    @{ Path = $activity; Pattern = 'releasingProjection'; Name = 'single projection release guard' },
    @{ Path = $activity; Pattern = 'stopProjection'; Name = 'projection stop request' },
    @{ Path = $service; Pattern = 'FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION'; Name = 'Android 14 projection foreground service' },
    @{ Path = $backend; Pattern = 'AndroidScreenCaptureSource'; Name = 'screen capture source' },
    @{ Path = $backend; Pattern = 'onProjectionFrame'; Name = 'frame routing' },
    @{ Path = $backend; Pattern = 'generation != generation_'; Name = 'generation-scoped frame routing' },
    @{ Path = $backend; Pattern = 'stopAsync'; Name = 'asynchronous capture stop' },
    @{ Path = $manifest; Pattern = 'CreatorStudioProjectionService'; Name = 'projection service manifest entry' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path)) {
        throw "missing Android projection frame file: $($check.Path)"
    }
    if (-not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android projection frame boundary: $($check.Name)"
    }
}
