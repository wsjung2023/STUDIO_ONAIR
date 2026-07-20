param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$renderer = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/AndroidTimelineExporter.java'
$engine = Join-Path $RepositoryRoot 'src/app/android/AndroidMediaCodecEditEngine.cpp'
$planner = Join-Path $RepositoryRoot 'src/app/android/AndroidTimelineExportPlan.cpp'
$main = Join-Path $RepositoryRoot 'src/main.cpp'

$checks = @(
    @{ Path = $renderer; Pattern = 'MediaMetadataRetriever'; Name = 'video timeline decoding' },
    @{ Path = $renderer; Pattern = 'MediaCodec\.createDecoderByType'; Name = 'audio timeline decoding' },
    @{ Path = $renderer; Pattern = 'drawVisual\('; Name = 'visual transform composition' },
    @{ Path = $renderer; Pattern = 'clip\.factor\(localUs\)'; Name = 'audio envelope mixing' },
    @{ Path = $renderer; Pattern = 'MediaMuxer'; Name = 'combined MP4 publication' },
    @{ Path = $renderer; Pattern = 'checkCancelled\(\)'; Name = 'cooperative cancellation' },
    @{ Path = $renderer; Pattern = 'CreatorStudioExport.*PASS'; Name = 'emulator runtime acceptance' },
    @{ Path = $activity; Pattern = 'creatorstudio\.exportSelfTest'; Name = 'debug export launch hook' },
    @{ Path = $engine; Pattern = 'preparePublication'; Name = 'durable publication evidence' },
    @{ Path = $engine; Pattern = 'publishAtomically'; Name = 'atomic export publication' },
    @{ Path = $planner; Pattern = 'generated overlay is missing or duplicated'; Name = 'no omitted generated text' },
    @{ Path = $planner; Pattern = 'media is offline'; Name = 'no omitted offline media' },
    @{ Path = $main; Pattern = 'AndroidMediaCodecEditEngine'; Name = 'Android export composition root' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path)) {
        throw "missing Android timeline export file: $($check.Path)"
    }
    if (-not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android timeline export boundary: $($check.Name)"
    }
}

Write-Host 'Android timeline export policy passed.'
