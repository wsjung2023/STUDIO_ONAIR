param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$resolver = Join-Path $RepositoryRoot 'src/app/android/AndroidExportDestinationResolver.cpp'
$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$worker = Join-Path $RepositoryRoot 'src/app/ExportWorker.cpp'

$checks = @(
    @{ Path = $resolver; Pattern = 'scheme\(\) != QStringLiteral\("content"\)'; Name = 'content URI validation' },
    @{ Path = $resolver; Pattern = 'CacheLocation'; Name = 'private render staging' },
    @{ Path = $activity; Pattern = 'getContentResolver\(\)\.openOutputStream'; Name = 'scoped-storage publication' },
    @{ Path = $activity; Pattern = 'try \(InputStream'; Name = 'stream cleanup boundary' },
    @{ Path = $worker; Pattern = 'RenderJobState::Publishing'; Name = 'uncancellable publication state' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path) -or
        -not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android scoped-storage boundary: $($check.Name)"
    }
}
