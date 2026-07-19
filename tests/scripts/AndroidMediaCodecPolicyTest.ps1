param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$encoder = Join-Path $RepositoryRoot 'src/app/android/AndroidMediaCodecSegmentEncoder.cpp'
$encoderHeader = Join-Path $RepositoryRoot 'src/app/android/AndroidMediaCodecSegmentEncoder.h'
$track = Join-Path $RepositoryRoot 'src/recorder/RecordingTrack.cpp'
$factory = Join-Path $RepositoryRoot 'src/app/LiveRecordingEngineFactory.cpp'
$appCmake = Join-Path $RepositoryRoot 'src/app/CMakeLists.txt'

$checks = @(
    @{ Path = $activity; Pattern = 'MediaCodec\.createEncoderByType'; Name = 'MediaCodec encoder ownership' },
    @{ Path = $activity; Pattern = 'new MediaMuxer'; Name = 'MediaMuxer container ownership' },
    @{ Path = $activity; Pattern = 'BUFFER_FLAG_END_OF_STREAM'; Name = 'codec EOS drain' },
    @{ Path = $activity; Pattern = 'getInputImage'; Name = 'stride-aware flexible YUV input' },
    @{ Path = $activity; Pattern = 'abortMediaEncoder'; Name = 'abort cleanup' },
    @{ Path = $activity; Pattern = 'CreatorStudioCodecSelfTest'; Name = 'debug runtime codec acceptance' },
    @{ Path = $encoderHeader; Pattern = 'AndroidMediaCodecSession'; Name = 'generation-bound native session' },
    @{ Path = $encoderHeader; Pattern = 'SegmentContainer::Mp4'; Name = 'typed MP4 publication' },
    @{ Path = $track; Pattern = '"mp4"'; Name = 'video MP4 extension' },
    @{ Path = $track; Pattern = '"m4a"'; Name = 'audio M4A extension' },
    @{ Path = $factory; Pattern = 'CS_APP_ENABLE_LIVE_RECORDING'; Name = 'Android live engine factory selection' },
    @{ Path = $appCmake; Pattern = 'CS_ENABLE_FFMPEG OR ANDROID'; Name = 'Android recording orchestration build' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path)) {
        throw "missing Android MediaCodec file: $($check.Path)"
    }
    if (-not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android MediaCodec boundary: $($check.Name)"
    }
}

Write-Host 'Android MediaCodec recording policy passed.'
