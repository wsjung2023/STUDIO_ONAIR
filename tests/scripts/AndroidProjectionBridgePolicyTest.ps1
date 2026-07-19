param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'

$javaActivity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$backendHeader = Join-Path $RepositoryRoot 'src/app/android/AndroidScreenCaptureBackend.h'
$backendSource = Join-Path $RepositoryRoot 'src/app/android/AndroidScreenCaptureBackend.cpp'
$mainSource = Join-Path $RepositoryRoot 'src/main.cpp'
$manifest = Join-Path $RepositoryRoot 'android/AndroidManifest.xml'

foreach ($path in @($javaActivity, $backendHeader, $backendSource, $mainSource, $manifest)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "missing Android projection bridge file: $path"
    }
}

$checks = @(
    @{ Path = $javaActivity; Pattern = 'class\s+CreatorStudioActivity\s+extends\s+QtActivity'; Name = 'custom Qt activity' },
    @{ Path = $javaActivity; Pattern = 'nativeProjectionResult'; Name = 'projection result JNI callback' },
    @{ Path = $javaActivity; Pattern = 'requestProjection'; Name = 'projection consent request' },
    @{ Path = $backendHeader; Pattern = 'makeAndroidScreenCaptureBackend'; Name = 'Android backend factory declaration' },
    @{ Path = $backendSource; Pattern = 'makeAndroidScreenCaptureBackend'; Name = 'Android backend factory implementation' },
    @{ Path = $mainSource; Pattern = '#elif\s+defined\(ANDROID\)'; Name = 'Android application backend selection' },
    @{ Path = $manifest; Pattern = 'CreatorStudioActivity'; Name = 'custom Android activity manifest entry' }
)

foreach ($check in $checks) {
    if (-not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android projection bridge boundary: $($check.Name)"
    }
}
